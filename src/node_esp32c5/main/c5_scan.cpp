#include "c5_scan.h"

#include <math.h>

namespace c5 {

Scanner::Scanner(RadioPort& radio) : radio_(radio)
{
    configureMeter(32, 8);
}

bool Scanner::configureMeter(int taps, int hop)
{
    // LO below the pilot: the pilot is at +10 MHz of RF above the LO, which
    // appears at kSpectrumSign * 10 MHz in baseband. LO above: the opposite.
    const bool ok = meterBelow_.configure(taps, hop, kSpectrumSign) &&
                    meterAbove_.configure(taps, hop, -kSpectrumSign);
    dirty_ = true;
    return ok;
}

void Scanner::setSeatFreq(int seat, uint16_t mhz)
{
    if (seat < 0 || seat >= kMaxSeats)
        return;
    if (!supported(mhz))
        mhz = 0;
    if (seatMhz_[seat] != mhz) {
        seatMhz_[seat] = mhz;
        dirty_ = true;
    }
}

float Scanner::frontEndDb(float offsetMhz)
{
    // A model, not a measurement beyond 38 MHz: a 5th-order roll-off fitted
    // to the bench's -3 dB near 21 MHz and ~-15 dB at 30 MHz. What matters
    // here is the ranking of the two sides, not the absolute figure; check
    // the alias paths on the bench (README.md, bench checklist). Only called when
    // the seat plan changes.
    const float r = (offsetMhz < 0 ? -offsetMhz : offsetMhz) / 22.0f;
    const float r2 = r * r, r5 = r2 * r2 * r;
    return -10.0f * log10f(1.0f + r5 * r5);
}

float Scanner::leakDb(uint16_t pilot, int side, const uint16_t* others, int nOthers)
{
    const int lo = pilot + side * kOffsetMhz;
    const int p = kSpectrumSign * (pilot - lo);   // where the pilot lands in baseband
    float worst = -200.0f;
    for (int i = 0; i < nOthers; ++i) {
        const uint16_t g = others[i];
        if (g == pilot)
            continue;
        const int delta = (int)g - lo;
        const int b = kSpectrumSign * delta;     // where the other VTX lands
        const float fe = frontEndDb((float)delta);
        // direct (k = 0) is the FIR's job; aliases k = +-1, +-2, and the image of each
        for (int k = -2; k <= 2; ++k) {
            if (k != 0) {
                const int d = b + 80 * k - p;
                if (d > -kFoldMhz && d < kFoldMhz && fe > worst)
                    worst = fe;
            }
            const int d = -b + 80 * k - p;
            if (d > -kFoldMhz && d < kFoldMhz && fe - kImageRejectDb > worst)
                worst = fe - kImageRejectDb;
        }
    }
    return worst;
}

int Scanner::chooseSide(uint16_t pilot, const uint16_t* others, int nOthers)
{
    const bool belowOk = pilot - kOffsetMhz >= kMinLoMhz;
    const bool aboveOk = pilot + kOffsetMhz <= kMaxLoMhz;
    if (!belowOk)
        return 1;
    if (!aboveOk)
        return -1;
    const float below = leakDb(pilot, -1, others, nOthers);
    const float above = leakDb(pilot, 1, others, nOthers);
    return above < below - 1.0f ? 1 : -1;   // LO below unless above is clearly better
}

void Scanner::rebuild()
{
    // distinct frequencies in use, ascending
    uint16_t freqs[kMaxSeats];
    int n = 0;
    for (int s = 0; s < kMaxSeats; ++s) {
        const uint16_t f = seatMhz_[s];
        if (!f)
            continue;
        int i = 0;
        while (i < n && freqs[i] < f)
            ++i;
        if (i < n && freqs[i] == f)
            continue;
        for (int j = n; j > i; --j)
            freqs[j] = freqs[j - 1];
        freqs[i] = f;
        ++n;
    }
    Visit fresh[kMaxSeats];
    for (int i = 0; i < n; ++i) {
        Visit& v = fresh[i];
        v.pilotMhz = freqs[i];
        v.side = (int8_t)chooseSide(freqs[i], freqs, n);
        v.loMhz = (uint16_t)(freqs[i] + v.side * kOffsetMhz);
        v.meter = v.side < 0 ? &meterBelow_ : &meterAbove_;
        for (int s = 0; s < kMaxSeats; ++s)
            if (seatMhz_[s] == freqs[i])
                v.seatMask |= (uint8_t)(1u << s);
        // keep what a visit on the same pilot and LO already learned
        for (int j = 0; j < nVisits_; ++j)
            if (visits_[j].pilotMhz == v.pilotMhz && visits_[j].loMhz == v.loMhz) {
                v.image = visits_[j].image;
                v.lastDbQ8 = visits_[j].lastDbQ8;
                v.haveLast = visits_[j].haveLast;
            }
    }
    for (int i = 0; i < kMaxSeats; ++i)
        visits_[i] = fresh[i];
    nVisits_ = n;
    dirty_ = false;
}

int32_t CalTable::gainAt(int mhz) const
{
    if (!n)
        return 0;
    if (mhz <= p[0].mhz)
        return p[0].gainDbQ8;
    for (int i = 1; i < n; ++i)
        if (mhz <= p[i].mhz) {
            const int span = p[i].mhz - p[i - 1].mhz;
            return p[i - 1].gainDbQ8 + (p[i].gainDbQ8 - p[i - 1].gainDbQ8) * (mhz - p[i - 1].mhz) / span;
        }
    return p[n - 1].gainDbQ8;
}

int32_t CalTable::floorAt(int mhz, int32_t fallback) const
{
    // nearest point that has a measured floor (INT16_MIN = none)
    int best = -1, bestD = 1 << 30;
    for (int i = 0; i < n; ++i) {
        if (p[i].floorDbQ8 == INT16_MIN)
            continue;
        const int d = mhz > p[i].mhz ? mhz - p[i].mhz : p[i].mhz - mhz;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best < 0 ? fallback : p[best].floorDbQ8;
}

static int calInsert(CalTable& t, int mhz)
{
    for (int i = 0; i < t.n; ++i)
        if (t.p[i].mhz == mhz)
            return i;
    if (t.n >= kMaxCalPoints)
        return -1;
    int i = t.n;
    while (i > 0 && t.p[i - 1].mhz > mhz) {
        t.p[i] = t.p[i - 1];
        --i;
    }
    t.p[i] = CalPoint{(uint16_t)mhz, 0, INT16_MIN};
    ++t.n;
    return i;
}

bool CalTable::set(int mhz, int32_t gainDbQ8)
{
    const int i = calInsert(*this, mhz);
    if (i < 0)
        return false;
    p[i].gainDbQ8 = (int16_t)gainDbQ8;
    return true;
}

bool CalTable::setFloor(int mhz, int32_t floorDbQ8)
{
    const int i = calInsert(*this, mhz);
    if (i < 0)
        return false;
    p[i].floorDbQ8 = (int16_t)floorDbQ8;
    return true;
}

int32_t correctDbQ8(int32_t dbQ8, int32_t floorDbQ8, int32_t gainDbQ8)
{
    // Leave the floor alone (noise near it is neither lifted nor flattened),
    // take the gain off as a plain dB offset well above it, and fade between
    // the two. The fade is at least |gain| + 1 dB wide so the output never
    // falls while the input rises.
    const int32_t r = dbQ8 - floorDbQ8;
    if (gainDbQ8 == 0 || r <= kCalFadeStartDbQ8)
        return dbQ8;
    int32_t width = (gainDbQ8 < 0 ? -gainDbQ8 : gainDbQ8) + 256;
    if (width < kCalFadeWidthDbQ8)
        width = kCalFadeWidthDbQ8;
    const int32_t into = r - kCalFadeStartDbQ8;
    if (into >= width)
        return dbQ8 - gainDbQ8;
    return dbQ8 - (int32_t)((int64_t)gainDbQ8 * into / width);
}

int32_t Scanner::corrected(int mhz, int32_t dbQ8) const
{
    if (!cal_.enabled || !cal_.n)
        return dbQ8;
    return correctDbQ8(dbQ8, cal_.floorAt(mhz, cfg_.noiseFloorDbQ8), cal_.gainAt(mhz));
}

int Scanner::rssiCounts(int32_t dbQ8, bool fullResolution) const
{
    int32_t c = (int32_t)(((int64_t)(dbQ8 - cfg_.floorDbQ8) * cfg_.countsPerDb) >> 8);
    if (fullResolution) {
        if (c < 1) c = 1;
        if (c > 4094) c = 4094;
        return c;
    }
    c >>= 4;
    if (c < 1) c = 1;
    if (c > 254) c = 254;
    return c;
}

void Scanner::cycle(uint32_t (*nowUs)())
{
    if (dirty_)
        rebuild();
    const uint32_t t0 = nowUs();
    int n = cfg_.captureN;
    if (n > kMaxCapture)
        n = kMaxCapture;
    for (int i = 0; i < nVisits_; ++i) {
        Visit& v = visits_[i];
        radio_.hop(v.loMhz);
        radio_.ensureGain(cfg_.gain);
        int64_t tUs = 0;
        const bool ok = radio_.capture(n, &tUs);
        ++stats_.captures;
        int32_t db = v.lastDbQ8;
        bool held = true;
        if (!ok) {
            ++stats_.failed;
        } else {
            int32_t br = 0, bi = 0;
            if (cfg_.image == ImageMode::Cancel)
                v.image.beta(&br, &bi);
            const MeterResult r = v.meter->measure(radio_.words(), n, br, bi);
            lastRes_[i] = r;
            lastTUs_[i] = tUs;
            v.image.add(r);
            if (r.clipped)
                ++stats_.clippedCaptures;
            const bool outOfBand = r.clipped >= cfg_.holdClipMin &&
                                   r.wideDbQ8 - r.bandDbQ8 >= cfg_.holdOobDbQ8;
            if (outOfBand && v.haveLast) {
                ++stats_.held;
            } else {
                db = r.bandDbQ8;
                held = false;
                v.lastDbQ8 = db;
                v.haveLast = true;
            }
        }
        if (!v.haveLast)
            continue;   // nothing good to report yet
        if (sink_) {
            const uint32_t tMs = (uint32_t)(tUs / 1000);   // same clock as millis()
            db = corrected(v.pilotMhz, db);
            for (int s = 0; s < kMaxSeats; ++s)
                if (v.seatMask & (1u << s))
                    sink_(sinkCtx_, s, tMs, db, held);
        }
    }
    const uint32_t dt = nowUs() - t0;
    stats_.lastCycleUs = dt;
    if (dt > stats_.maxCycleUs)
        stats_.maxCycleUs = dt;
    ++stats_.cycles;
}

}  // namespace c5
