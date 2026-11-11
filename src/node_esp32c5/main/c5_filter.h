// A running median for RotorHazard's RssiNode whose length is set at run
// time, so the C5 node can smooth over a fixed *time* window.
//
// RotorHazard's own filter is a 255-reading median. On an RX5808 node each
// seat is read about once per millisecond, so that is ~255 ms. On the C5 one
// radio visits the seats in turn and a seat gets only a few hundred readings
// a second, so 255 readings became ~0.6 s and fast passes were lost
// (docs/esp32c5-fast-pass-detection.md in the laptimer repo). This filter
// keeps RotorHazard's behaviour (median of the last N readings, timestamp of
// the middle reading) with N chosen from the window and the measured rate.
//
// Portable (host tests use it). Buffers are on the heap: on the C5, static
// RAM must stay clear of the capture bank.
#ifndef RHNODE_C5_FILTER_H
#define RHNODE_C5_FILTER_H

#include <stdint.h>
#include <stdlib.h>

#include "c5_meter.h"
#include "util/filter.h"

namespace c5 {

constexpr int kMaxMedian = 255;
constexpr int kMaxPowerMean = 16;   // the minimum-pass policy uses up to 12

// The live fast-pass filter operates on dB before RotorHazard RSSI mapping,
// so its arithmetic is a real linear-power mean.  Applying an arithmetic
// mean to RSSI counts would instead average logarithms (a geometric mean in
// power) and would not match the recorded-trace experiments.
class ShortPowerMean {
public:
    void setLength(int n)
    {
        if (n < 1) n = 1;
        if (n > kMaxLength) n = kMaxLength;
        n_ = n;
        reset();
    }
    int length() const { return n_; }
    void reset() { count_ = head_ = 0; }

    // Held/failed captures break the window.  Return true only when a fresh
    // complete output is available.  The output timestamp is the strongest
    // raw sample in the window, preserving pass-peak timing.
    bool add(mtime_t ts, int32_t dbQ8, bool held, mtime_t* outTs, int32_t* outDbQ8)
    {
        if (held) {
            reset();
            return false;
        }
        db_[head_] = dbQ8;
        ts_[head_] = ts;
        power_[head_] = powerFromDbQ8(dbQ8);
        head_ = (head_ + 1) % n_;
        if (count_ < n_) ++count_;
        if (count_ < n_) return false;

        uint64_t sum = 0;
        int strongest = 0;
        for (int i = 0; i < n_; ++i) {
            sum += power_[i];
            if (db_[i] > db_[strongest]) strongest = i;
        }
        *outDbQ8 = dbQ8FromPower(sum / (uint64_t)n_) - dbQ8FromPower(1u << 20);
        *outTs = ts_[strongest];
        return true;
    }

private:
    static constexpr int kMaxLength = kMaxPowerMean;
    uint64_t power_[kMaxLength] = {};
    int32_t db_[kMaxLength] = {};
    mtime_t ts_[kMaxLength] = {};
    int n_ = 2, count_ = 0, head_ = 0;
};

// RssiNode still owns crossing/history logic.  In power-mean mode it receives
// already-smoothed values through this timestamp-preserving pass-through.
class LatestFilter : public Filter<rssi_t> {
public:
    bool isFilled() override { return have_; }
    void addRawValue(mtime_t ts, rssi_t value) override { ts_ = ts; value_ = value; have_ = true; }
    rssi_t getFilteredValue() override { return value_; }
    mtime_t getFilterTimestamp() override { return ts_; }
    void reset() { have_ = false; }
private:
    bool have_ = false;
    rssi_t value_ = 0;
    mtime_t ts_ = 0;
};

class RuntimeMedian : public Filter<rssi_t> {
public:
    RuntimeMedian()
    {
        vals_ = (rssi_t*)malloc(sizeof(rssi_t) * kMaxMedian);
        sorted_ = (rssi_t*)malloc(sizeof(rssi_t) * kMaxMedian);
        times_ = (mtime_t*)malloc(sizeof(mtime_t) * kMaxMedian);
        setLength(kMaxMedian);
    }
    ~RuntimeMedian()
    {
        free(vals_);
        free(sorted_);
        free(times_);
    }
    RuntimeMedian(const RuntimeMedian&) = delete;
    RuntimeMedian& operator=(const RuntimeMedian&) = delete;

    // An odd length, 1..255. Starts filling again (as after power-up).
    void setLength(int n)
    {
        if (n < 1)
            n = 1;
        if (n > kMaxMedian)
            n = kMaxMedian;
        if (!(n & 1))
            --n;
        n_ = n;
        count_ = 0;
        head_ = 0;
    }
    int length() const { return n_; }
    bool ok() const { return vals_ && sorted_ && times_; }

    bool isFilled() override { return count_ >= n_; }

    void addRawValue(mtime_t ts, rssi_t value) override
    {
        if (!ok())
            return;
        int filled = count_ < n_ ? count_ : n_;
        if (count_ >= n_) {
            // drop the oldest from the sorted copy
            const rssi_t old = vals_[head_];
            int i = 0;
            while (i < filled && sorted_[i] != old)
                ++i;
            for (; i < filled - 1; ++i)
                sorted_[i] = sorted_[i + 1];
            --filled;
        }
        vals_[head_] = value;
        times_[head_] = ts;
        head_ = (head_ + 1) % n_;
        if (count_ < n_)
            ++count_;
        // insert into the sorted copy
        int i = filled;
        while (i > 0 && sorted_[i - 1] > value) {
            sorted_[i] = sorted_[i - 1];
            --i;
        }
        sorted_[i] = value;
    }

    rssi_t getFilteredValue() override
    {
        const int filled = count_ < n_ ? count_ : n_;
        return filled ? sorted_[filled / 2] : 0;
    }

    // The time of the middle reading of the window, as RotorHazard's filter.
    mtime_t getFilterTimestamp() override
    {
        const int filled = count_ < n_ ? count_ : n_;
        if (!filled)
            return 0;
        const int back = filled / 2;              // readings before the newest
        const int newest = (head_ + n_ - 1) % n_;
        return times_[(newest - back + n_) % n_];
    }

private:
    rssi_t* vals_ = nullptr;     // ring of the last n_ readings
    rssi_t* sorted_ = nullptr;   // the same readings, sorted
    mtime_t* times_ = nullptr;
    int n_ = kMaxMedian, count_ = 0, head_ = 0;
};

// Readings that make up windowMs at this per-seat rate (odd, 1..255).
inline int medianLengthFor(int windowMs, float readingsPerSecond)
{
    int n = (int)(windowMs * readingsPerSecond / 1000.0f + 0.5f);
    if (n < 1)
        n = 1;
    if (n > kMaxMedian)
        n = kMaxMedian;
    if (!(n & 1))
        n = n < kMaxMedian ? n + 1 : n - 1;
    return n;
}

}  // namespace c5

#endif
