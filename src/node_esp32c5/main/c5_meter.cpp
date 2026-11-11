#include "c5_meter.h"

#include <math.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define C5_HOT IRAM_ATTR __attribute__((optimize("O3")))
#else
#define C5_HOT
#endif

namespace c5 {

namespace {

// log2(1 + i/256) in Q16 for i = 0..256. On the heap: static RAM must stay
// below the capture bank (tools/check_phy.py).
const uint32_t* log2Table()
{
    static uint32_t* table = nullptr;
    if (!table) {
        uint32_t* t = (uint32_t*)malloc(257 * sizeof(uint32_t));
        for (int i = 0; i <= 256; ++i)
            t[i] = (uint32_t)lround(log2(1.0 + i / 256.0) * 65536.0);
        table = t;
    }
    return table;
}

int32_t log2Q16(uint64_t v)
{
    const uint32_t* t = log2Table();
    const int e = 63 - __builtin_clzll(v);
    uint32_t f;  // fraction of the mantissa, 16 bits
    if (e >= 16)
        f = (uint32_t)(v >> (e - 16)) & 0xFFFF;
    else
        f = (uint32_t)(v << (16 - e)) & 0xFFFF;
    const uint32_t i = f >> 8, r = f & 0xFF;
    const uint32_t frac = t[i] + (((t[i + 1] - t[i]) * r) >> 8);
    return e * 65536 + (int32_t)frac;
}

double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum)
            break;
    }
    return sum;
}

// 10*log10(2) * 256 / 65536, as a Q32 multiplier: dbQ8 = (log2Q16 * K) >> 32.
constexpr int64_t kDbPerLog2Q16 = (int64_t)(3.0102999566398120 * 256.0 / 65536.0 * 4294967296.0 + 0.5);

}  // namespace

int32_t dbQ8FromPower(uint64_t v)
{
    if (v == 0)
        return -100 * 256;
    return (int32_t)(((int64_t)log2Q16(v) * kDbPerLog2Q16) >> 32);
}

uint64_t powerFromDbQ8(int32_t dbQ8)
{
    // 10^(d/10) = 2^(d * log2(10) / 10); in Q16 of the exponent
    static uint32_t* table = nullptr;   // 2^(i/256) in Q24, on the heap (see log2Table)
    if (!table) {
        uint32_t* t = (uint32_t*)malloc(257 * sizeof(uint32_t));
        for (int i = 0; i <= 256; ++i)
            t[i] = (uint32_t)lround(exp2(i / 256.0) * 16777216.0);
        table = t;
    }
    if (dbQ8 > 60 * 256)
        dbQ8 = 60 * 256;
    if (dbQ8 < -60 * 256)
        dbQ8 = -60 * 256;
    // exponent in Q16: dbQ8 / 256 * 0.33219281 * 65536 = dbQ8 * 85.0414
    const int64_t e = ((int64_t)dbQ8 * 87082) >> 10;   // 87082/1024 = 85.041
    const int64_t ip = e >> 16;                          // floor
    const uint32_t frac = (uint32_t)(e - (ip << 16));    // 0..65535
    const uint32_t i = frac >> 8, r = frac & 0xFF;
    const uint64_t m = table[i] + ((((uint64_t)table[i + 1] - table[i]) * r) >> 8);   // Q24
    // 2^20 * 2^ip * m / 2^24
    const int sh = (int)ip + 20 - 24;
    return sh >= 0 ? m << sh : m >> -sh;
}

bool BandMeter::configure(int taps, int hop, int pilotSign, float cutoffMhz, float beta)
{
    if (taps < kMixPeriod || taps > kMaxTaps || taps % kMixPeriod || hop < kMixPeriod || hop % kMixPeriod)
        return false;
    taps_ = taps;
    hop_ = hop;
    sign_ = pilotSign >= 0 ? 1 : -1;

    // Windowed-sinc low-pass, normalised to unity gain at DC, then shifted to
    // the pilot. Coefficient n multiplies sample (k*hop + n); hop is a
    // multiple of the mixer period, so one coefficient set serves every k.
    const double fc = (double)cutoffMhz / kSampleRateMhz;   // cycles per sample
    const double mid = (taps - 1) / 2.0;
    const double i0b = besselI0(beta);
    double h[kMaxTaps], sum = 0;
    for (int n = 0; n < taps; ++n) {
        const double t = n - mid;
        const double sinc = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
        const double r = 2.0 * n / (taps - 1) - 1.0;
        const double w = besselI0(beta * sqrt(1.0 - r * r)) / i0b;
        h[n] = sinc * w;
        sum += h[n];
    }
    sumCr_ = sumCi_ = 0;
    for (int n = 0; n < taps; ++n) {
        h[n] /= sum;
        hReal_[n] = (float)h[n];
        // shift the pilot (at sign * 10 MHz) down to 0 Hz
        const double ph = -2 * M_PI * sign_ * (double)kOffsetMhz / kSampleRateMhz * n;
        cr_[n] = (int16_t)lround(h[n] * cos(ph) * 16384.0);
        ci_[n] = (int16_t)lround(h[n] * sin(ph) * 16384.0);
        sumCr_ += cr_[n];
        sumCi_ += ci_[n];
    }
    log2Table();
    return true;
}

float BandMeter::responseDb(float offsetMhz) const
{
    double re = 0, im = 0;
    for (int n = 0; n < taps_; ++n) {
        const double ph = -2 * M_PI * offsetMhz / kSampleRateMhz * n;
        re += hReal_[n] * cos(ph);
        im += hReal_[n] * sin(ph);
    }
    const double p = re * re + im * im;
    return p > 0 ? (float)(10 * log10(p)) : -200.0f;
}

C5_HOT MeterResult BandMeter::measure(const volatile uint32_t* words, int n,
                                      int32_t imageBetaRe, int32_t imageBetaIm) const
{
    MeterResult r;
    if (taps_ == 0 || n < taps_)
        return r;

    // Pass 1: moments and clipping.
    int32_t sI = 0, sQ = 0;
    int64_t sII = 0, sQQ = 0, sIQ = 0;
    uint16_t clipped = 0;
    for (int k = 0; k < n; ++k) {
        const uint32_t w = words[k];
        const int i = sampleI(w), q = sampleQ(w);
        sI += i;
        sQ += q;
        sII += i * i;
        sQQ += q * q;
        sIQ += i * q;
        if (i >= 511 || i <= -512 || q >= 511 || q <= -512)
            ++clipped;
    }
    r.clipped = clipped;
    // central moments
    const int64_t cII = sII - ((int64_t)sI * sI) / n;
    const int64_t cQQ = sQQ - ((int64_t)sQ * sQ) / n;
    const int64_t cIQ = sIQ - ((int64_t)sI * sQ) / n;
    r.m2re = (int32_t)((cII - cQQ) >> kMomentShift);
    r.m2im = (int32_t)((2 * cIQ) >> kMomentShift);
    r.m2abs = (uint32_t)((cII + cQQ) >> kMomentShift);
    r.wideDbQ8 = dbQ8FromPower((uint64_t)(cII + cQQ)) - dbQ8FromPower((uint64_t)n);

    // DC response of the pilot and mirror filters, for the capture mean (Q8).
    const int64_t mI8 = ((int64_t)sI * 256) / n, mQ8 = ((int64_t)sQ * 256) / n;
    const int32_t dcR = (int32_t)((mI8 * sumCr_ - mQ8 * sumCi_) >> 8);
    const int32_t dcI = (int32_t)((mI8 * sumCi_ + mQ8 * sumCr_) >> 8);
    const int32_t dcMR = (int32_t)((mI8 * sumCr_ + mQ8 * sumCi_) >> 8);
    const int32_t dcMI = (int32_t)((mQ8 * sumCr_ - mI8 * sumCi_) >> 8);
    const bool image = imageBetaRe != 0 || imageBetaIm != 0;

    // Pass 2: the FIR at every hop. Four real sums give both the pilot band
    // (A - B, C + D) and its mirror (A + B, C - D) for the same multiplies.
    uint64_t acc = 0;
    int outputs = 0;
    for (int k = 0; k + taps_ <= n; k += hop_) {
        int32_t a = 0, b = 0, c = 0, d = 0;
        const volatile uint32_t* x = words + k;
        for (int t = 0; t < taps_; ++t) {
            const uint32_t w = x[t];
            const int32_t i = sampleI(w), q = sampleQ(w);
            a += cr_[t] * i;
            b += ci_[t] * q;
            c += cr_[t] * q;
            d += ci_[t] * i;
        }
        int32_t yr = a - b - dcR, yi = c + d - dcI;
        if (image) {
            const int64_t mr = a + b - dcMR, mi = c - d - dcMI;
            // y -= nu * conj(mirror)
            yr -= (int32_t)((imageBetaRe * mr + imageBetaIm * mi) >> 15);
            yi -= (int32_t)((imageBetaIm * mr - imageBetaRe * mi) >> 15);
        }
        yr >>= 6;
        yi >>= 6;
        acc += (uint64_t)((int64_t)yr * yr) + (uint64_t)((int64_t)yi * yi);
        ++outputs;
    }
    // acc is in (LSB * 2^8)^2 per output: remove 2^16 and the output count.
    constexpr int32_t kDb2p16Q8 = 12330;  // 10*log10(65536) * 256
    r.bandDbQ8 = dbQ8FromPower(acc) - dbQ8FromPower((uint64_t)outputs) - kDb2p16Q8;
    return r;
}

void ImageEstimator::add(const MeterResult& r)
{
    // exponential average over ~256 captures
    re_ += r.m2re - (re_ >> 8);
    im_ += r.m2im - (im_ >> 8);
    abs_ += r.m2abs - (abs_ >> 8);
    if (n_ < 0xFFFFFFFF)
        ++n_;
}

void ImageEstimator::beta(int32_t* re, int32_t* im) const
{
    *re = *im = 0;
    if (n_ < 256 || abs_ == 0)
        return;
    const int64_t den = 2 * (int64_t)abs_;
    int64_t br = (re_ * 32768) / den, bi = (im_ * 32768) / den;
    const int64_t lim = 8192;  // |nu| <= 0.25: anything larger is not imbalance
    if (br > lim) br = lim;
    if (br < -lim) br = -lim;
    if (bi > lim) bi = lim;
    if (bi < -lim) bi = -lim;
    *re = (int32_t)br;
    *im = (int32_t)bi;
}

}  // namespace c5
