// Band power of one pilot from one raw I/Q capture of the ESP32-C5 radio.
//
// The radio is tuned so the pilot sits 10 MHz off the LO. The meter is a
// complex band-pass FIR centred on the pilot (a windowed-sinc low-pass
// shifted to the pilot's offset), evaluated every `hop` samples across the
// capture; its mean output power, in dB, is the pilot's reading.
//
// Selectivity is the point of the FIR: a strong VTX one Raceband channel
// away (37 MHz) must not read as a pass on its neighbour. The default
// 32-tap Kaiser design (cutoff 7.5 MHz, its -6 dB point) is within 1 dB to
// about +-4.5 MHz, where most of an analogue video signal's energy is, and
// down by 50 dB or more beyond about +-14 MHz.
//
// Optional image cancellation: I/Q imbalance in the receiver puts a weak
// copy of whatever sits at the pilot's mirror (the other side of the LO)
// onto the pilot. The meter can also evaluate the mirror band and subtract
// beta * conj(mirror), with beta estimated blind from the captures
// themselves (see ImageEstimator).
//
// Portable: no ESP-IDF here, so the host tests run this exact code.
#ifndef RHNODE_C5_METER_H
#define RHNODE_C5_METER_H

#include <stdint.h>
#include <stddef.h>

namespace c5 {

constexpr int kSampleRateMhz = 80;   // the capture engine's rate code 0
constexpr int kOffsetMhz = 10;       // pilot offset from the LO
constexpr int kMixPeriod = kSampleRateMhz / kOffsetMhz;  // 8 samples
constexpr int kMaxTaps = 64;

// Decode one capture word: bits 0-9 signed I, 10-19 signed Q, 20-26 gain.
static inline int sampleI(uint32_t w) { return (int)((int32_t)(w << 22) >> 22); }
static inline int sampleQ(uint32_t w) { return (int)((int32_t)(w << 12) >> 22); }
static inline int sampleGain(uint32_t w) { return (int)((w >> 20) & 0x7F); }
static inline bool sampleClipped(uint32_t w)
{
    const int i = sampleI(w), q = sampleQ(w);
    return i >= 511 || i <= -512 || q >= 511 || q <= -512;
}

// 10*log10(v) in Q8 dB (1/256 dB) for v > 0; a large negative value for 0.
int32_t dbQ8FromPower(uint64_t v);
// 2^20 * 10^(dB/10) for dB (Q8) in [-60, +60]: the inverse of dbQ8FromPower
// on a 2^20 scale. Fixed point (the C5 has no FPU).
uint64_t powerFromDbQ8(int32_t dbQ8);

struct MeterResult {
    int32_t bandDbQ8 = 0;    // pilot band power, dB re 1 LSB^2, Q8
    int32_t wideDbQ8 = 0;    // whole-capture power after DC removal, same units
    uint16_t clipped = 0;    // samples at the ADC rails
    // For the image estimator: sums over the capture of (x - mean)^2 (complex)
    // and |x - mean|^2, scaled down by 2^kMomentShift.
    int32_t m2re = 0, m2im = 0;
    uint32_t m2abs = 0;
};

constexpr int kMomentShift = 6;

class BandMeter {
public:
    // taps: FIR length (8..64, multiple of 8). hop: output spacing in samples
    // (multiple of 8, so the mixer phase repeats). pilotSign: +1 if the
    // pilot appears at +10 MHz in the baseband capture, -1 at -10 MHz.
    // cutoffMhz: the -6 dB half-width. beta: Kaiser window shape.
    bool configure(int taps, int hop, int pilotSign, float cutoffMhz = 7.5f, float beta = 6.0f);

    // One capture of n words. imageBetaQ15 (re, im): if non-zero, cancel the
    // image with that coefficient (costs a second FIR pass).
    MeterResult measure(const volatile uint32_t* words, int n,
                        int32_t imageBetaRe = 0, int32_t imageBetaIm = 0) const;

    int taps() const { return taps_; }
    int hop() const { return hop_; }
    int pilotSign() const { return sign_; }

    // Power gain of the meter at a frequency offset from the pilot, in dB
    // (0 at the centre). For tests and the console's filter report.
    float responseDb(float offsetMhz) const;

private:
    int taps_ = 0, hop_ = 8, sign_ = -1;
    int16_t cr_[kMaxTaps] = {}, ci_[kMaxTaps] = {};   // Q14 complex coefficients
    int32_t sumCr_ = 0, sumCi_ = 0;                     // their sums (DC response)
    float hReal_[kMaxTaps] = {};                         // the real prototype (for responseDb)
};

// Blind I/Q-imbalance estimate for one seat. A receiver with imbalance
// turns an ideal (circular) signal z into x = z + nu * conj(z); for circular
// z, E[x^2] = 2 nu E[|x|^2], so nu follows from two running moments. The
// moments average over many captures, so a strong pilot (which rotates at
// twice its offset and averages out) does not bias it.
class ImageEstimator {
public:
    void reset() { re_ = im_ = 0; abs_ = 0; n_ = 0; }
    void add(const MeterResult& r);
    // nu in Q15, (0, 0) until enough captures have been seen.
    void beta(int32_t* re, int32_t* im) const;
    uint32_t count() const { return n_; }

private:
    int64_t re_ = 0, im_ = 0;
    uint64_t abs_ = 0;
    uint32_t n_ = 0;
};

}  // namespace c5

#endif
