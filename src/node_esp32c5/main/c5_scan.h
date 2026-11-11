// Seat scheduling for the ESP32-C5 receiver: one radio measures every seat
// in turn, one capture per distinct frequency per cycle.
//
// Ghosting defences, in the order a reading meets them:
//
// 1. LO side. The pilot sits 10 MHz above or below the LO. Another seat's
//    VTX can fold onto it three ways: through the image (I/Q imbalance
//    mirrors the other side of the LO, about 18 dB down on chip v1.0),
//    through aliasing (the capture samples at 80 MS/s, so a VTX 80 MHz from
//    where the pilot lands folds onto it; only the analogue front end stops
//    that), or both. The side is chosen per seat to minimise the worst
//    expected leak from the other seats' pilots (leakDb()).
// 2. Selectivity: the meter's FIR rejects neighbours (c5_meter.h).
// 3. Image cancellation (optional, ImageMode::Cancel): for plans where both
//    images are occupied.
// 4. Out-of-band overload hold. A strong VTX on another channel can drive
//    the ADC into clipping while the LO is tuned for this seat; the clipped
//    capture smears its energy across the band and this seat would read a
//    pass that is not there. A capture that clips while the pilot band holds
//    only a small part of its power is not used: the seat keeps its last
//    good reading for that cycle.
//
// Portable: the radio is behind RadioPort, so the host tests drive this
// exact code.
#ifndef RHNODE_C5_SCAN_H
#define RHNODE_C5_SCAN_H

#include <stdint.h>

#include "c5_meter.h"

namespace c5 {

constexpr int kMaxSeats = 8;
constexpr int kMinPilotMhz = 5180;   // LO range checked on chip v1.0: 5170-5917
constexpr int kMaxPilotMhz = 5917;
constexpr int kMinLoMhz = 5170;
constexpr int kMaxLoMhz = 5917;
constexpr int kFoldMhz = 12;         // a fold landing this close to the pilot leaks into its band
constexpr float kImageRejectDb = 18.0f;   // I/Q image rejection, chip v1.0 at gain 30 (bench)
constexpr int kMaxCapture = 2048;

class RadioPort {
public:
    virtual ~RadioPort() {}
    // PLL-only retune to the LO; settled on return.
    virtual void hop(int loMhz) = 0;
    // Re-force the receive gain if the force has been lost.
    virtual void ensureGain(int index) = 0;
    // One capture of n words; tUs = the clock (64-bit, never wraps) at its
    // middle. False if the capture is incomplete (sentinel or guard check failed).
    virtual bool capture(int n, int64_t* tUs) = 0;
    virtual const volatile uint32_t* words() = 0;
};

enum class ImageMode : uint8_t { Off = 0, Cancel = 1 };

struct ScanConfig {
    int gain = 30;           // receive gain index
    int captureN = 256;      // samples per capture (3.2 us at 80 MS/s)
    ImageMode image = ImageMode::Off;
    // Out-of-band overload hold: at least this many clipped samples, and the
    // pilot band this many dB (Q8) or more below the whole capture.
    int holdClipMin = 4;
    int32_t holdOobDbQ8 = 10 * 256;
    // dB -> RotorHazard RSSI (kCountsPerDbQ4 counts per dB from here).
    int32_t floorDbQ8 = -8 * 256;
    // The noise floor assumed where the calibration table has none (dB Q8;
    // chip v1.0 at gain 30 reads about -3.2 dB with nothing on air).
    int32_t noiseFloorDbQ8 = -3 * 256 - 51;
};

// The fixed RSSI scale: four reported 8-bit RSSI counts per dB (Q4). Clients
// rely on it: one count is 0.25 dB of calibration adjustment.
constexpr int kCountsPerDbQ4 = 64;

// Per-board receiver calibration: the C5's gain at each frequency (relative,
// dB) and its noise floor there. A reading more than 10 dB above the floor
// has the gain taken off as a plain dB offset; within 3 dB of the floor it is
// left alone (the floor and its noise stay as they are, flat across the
// band); in between the offset fades in linearly. Points are interpolated
// linearly in MHz; outside them the nearest point applies.
constexpr int32_t kCalFadeStartDbQ8 = 3 * 256;
constexpr int32_t kCalFadeWidthDbQ8 = 7 * 256;
constexpr int kMaxCalPoints = 16;
struct CalPoint {
    uint16_t mhz;
    int16_t gainDbQ8;    // the C5 reads this much high here
    int16_t floorDbQ8;   // its noise floor here (measured with no VTX on)
};
struct CalTable {
    uint8_t n = 0;
    bool enabled = true;
    CalPoint p[kMaxCalPoints];
    int32_t gainAt(int mhz) const;
    int32_t floorAt(int mhz, int32_t fallback) const;
    bool set(int mhz, int32_t gainDbQ8);   // add or replace (keeps floor)
    bool setFloor(int mhz, int32_t floorDbQ8);
};
// Per-channel trims on top of the calibration table, at fixed frequencies:
// Lowband L1-L8 then Raceband R1-R8 (ascending). Positive raises the
// reported RSSI. Interpolated linearly in MHz between them, so the other
// bands get trimmed too; outside them the nearest trim applies. The table's
// enabled flag switches table and trims off together.
constexpr int kTrimPoints = 16;
extern const uint16_t kTrimMhz[kTrimPoints];
constexpr int32_t kMaxAdjustmentDbQ8 = 20 * 256;
struct TrimTable {
    int16_t adjDbQ8[kTrimPoints] = {};
    int32_t at(int mhz) const;
};
// Apply a gain correction to one reading (all Q8 dB).
int32_t correctDbQ8(int32_t dbQ8, int32_t floorDbQ8, int32_t gainDbQ8);

struct Visit {
    uint16_t pilotMhz = 0;
    uint16_t loMhz = 0;
    int8_t side = -1;        // -1: LO below the pilot, +1: above
    uint8_t seatMask = 0;
    BandMeter* meter = nullptr;
    ImageEstimator image;
    int32_t lastDbQ8 = 0;
    bool haveLast = false;
    uint32_t lastGoodUs = 0;
};

// One raw visit, including invalid visits; flags: 1 failed, 2 held, 4 clipped.
struct Observation {
    uint32_t tUs = 0, goodAgeUs = 0;
    int32_t bandDbQ8 = 0, wideDbQ8 = 0;
    uint16_t clipped = 0;
    uint8_t flags = 0, reserved = 0;
};

struct ScanStats {
    uint32_t cycles = 0, captures = 0, failed = 0, held = 0, clippedCaptures = 0;
    uint32_t lastCycleUs = 0, maxCycleUs = 0;
};

// One seat reading: seat index, C5 clock (ms) at the capture's middle, band
// power (dB Q8), and whether it is a held value (overload or failed capture).
typedef void (*SeatSink)(void* ctx, int seat, uint32_t tMs, int32_t dbQ8, bool held);

class Scanner {
public:
    explicit Scanner(RadioPort& radio);

    // 0 or 1111 turns a seat off; unsupported frequencies leave it off.
    void setSeatFreq(int seat, uint16_t mhz);
    uint16_t seatFreq(int seat) const { return seatMhz_[seat]; }
    static bool supported(uint16_t mhz) { return mhz >= kMinPilotMhz && mhz <= kMaxPilotMhz; }

    ScanConfig& config() { return cfg_; }
    TrimTable& trim() { return trim_; }
    // The receiver's total gain correction at a frequency (table minus trim,
    // limited to +-20 dB), dB Q8; 0 when calibration is off.
    int32_t gainAt(int mhz) const;
    // Apply a changed meter design or capture length.
    bool configureMeter(int taps, int hop);

    void setSink(SeatSink sink, void* ctx) { sink_ = sink; sinkCtx_ = ctx; }
    // One pass over every visit. nowUs: a clock for cycle timing.
    void cycle(uint32_t (*nowUs)());

    int visitCount() const { return nVisits_; }
    const Visit& visit(int i) const { return visits_[i]; }
    const ScanStats& stats() const { return stats_; }
    void clearStats() { stats_ = ScanStats(); }
    // The last raw measurement of visit i, for the console.
    const MeterResult& lastResult(int i) const { return lastRes_[i]; }
    const Observation& lastObservation(int i) const { return observations_[i]; }
    // The C5 clock (us) at the middle of visit i's last capture.
    int64_t lastTimeUs(int i) const { return lastTUs_[i]; }

    // dB (Q8) to upstream main's standard 8-bit RotorHazard scale.
    int rssiCounts(int32_t dbQ8) const;

    CalTable& cal() { return cal_; }
    // A reading at mhz as the sink gets it: gain-corrected when enabled.
    int32_t corrected(int mhz, int32_t dbQ8) const;

    // Pick the LO side for a pilot given the other pilots in use.
    static int chooseSide(uint16_t pilot, const uint16_t* others, int nOthers);
    // Worst expected leak (dB re the leaking VTX's own reading) from the
    // other pilots onto this pilot with the LO on this side; -200 if none.
    static float leakDb(uint16_t pilot, int side, const uint16_t* others, int nOthers);
    // Front-end model: analogue attenuation at an offset from the LO.
    static float frontEndDb(float offsetMhz);

private:
    RadioPort& radio_;
    ScanConfig cfg_;
    CalTable cal_;
    TrimTable trim_;
    BandMeter meterBelow_, meterAbove_;   // LO below (pilot at +10 MHz before inversion) / above
    uint16_t seatMhz_[kMaxSeats] = {};
    Visit visits_[kMaxSeats];
    MeterResult lastRes_[kMaxSeats];
    Observation observations_[kMaxSeats];
    int64_t lastTUs_[kMaxSeats] = {};
    int nVisits_ = 0;
    bool dirty_ = true;
    ScanStats stats_;
    SeatSink sink_ = nullptr;
    void* sinkCtx_ = nullptr;

    void rebuild();
};

// The baseband sign of the pilot: RF above the LO appears at negative
// frequency in the C5's I/Q (the spectrum is inverted). If a bench check
// shows the pilot on the wrong side, this is the one constant to flip.
constexpr int kSpectrumSign = -1;

}  // namespace c5

#endif
