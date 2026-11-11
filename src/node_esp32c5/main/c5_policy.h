// Minimum-pass detection policy: from the shortest pass the gate must catch,
// choose the receiver setting with the least RSSI jitter that still detects
// it reliably. The choice is smooth: one more or one fewer averaged reading.
//
// Measured on 26:A8, 11 November 2026 (laptimer
// docs/esp32c5-jitter-vs-speed-20261111.md):
//
// - Floor jitter falls only with I/Q samples averaged. One visit costs about
//   148 us (PLL hop) + 0.515 us per sample, so long captures are the cheapest
//   noise reduction: the reliable part of the curve uses 1024 samples and the
//   linear-power mean from 12 readings (about +-1 count) down to 3 (+-2).
// - Jitter (p0.5..p99.5 half-range, 8-bit counts) is 3.3 / sqrt(readings).
// - Detection: a 3-reading mean detected 30/30 down to a pass 0.77 x the
//   revisit time (8 channels: 0.2 m, 60 deg, 200 km/h). The policy keeps that
//   averaging-to-pass ratio: readings <= 3.9 x pass / revisit.
// - Past the 3-reading point the curve goes on, for experiments, with
//   shorter captures (faster revisit, much more jitter), flagged extreme:
//   detection not guaranteed.
//
// The revisit time depends on how many distinct frequencies one receiver
// scans, so the whole curve is reported back for the current channel plan.
//
// Portable: the host tests run this exact code.
#ifndef RHNODE_C5_POLICY_H
#define RHNODE_C5_POLICY_H

#include <stdint.h>

namespace c5 {

constexpr int kPolicyVisitFixedUs = 148;
constexpr int kPolicyVisitPerSampleNs = 515;
constexpr int kPolicySamples = 1024;
constexpr int kPolicyMaxMean = 12;
constexpr int kPolicyReadingsPerRevisitX10 = 39;   // readings <= 3.9 x pass / revisit

struct PolicyPoint {
    int samples;          // I/Q samples per capture
    int readings;         // linear-power mean length
    uint8_t jitterX10;    // floor jitter, +- counts x10 (p0.5..p99.5 half-range)
    uint8_t margin;       // safe enter level above the floor, counts
    bool extreme;         // experimental: reliable detection not guaranteed
};

// The curve from least to most jitter. Jitter: 3.3 / sqrt(readings) at 1024
// samples; 512 and 128 samples measured (mean 2: -5..+3 and -9..+8).
// Margins: zero source-off false laps in 120 s on eight seats, plus 2
// (measured at 12, 6, 3 readings and both extreme points; others follow the
// jitter). The extreme points trade jitter for a faster revisit: at 0.2 m,
// 20 deg, 200 km/h (1 ms) 128 samples + mean 2 detected 30/30 strong and
// 18/30 at -6 dB, the best of the settings tried.
constexpr PolicyPoint kPolicyCurve[] = {
    {1024, 12, 10, 5, false}, {1024, 11, 10, 5, false}, {1024, 10, 10, 5, false},
    {1024, 9, 11, 6, false},  {1024, 8, 12, 6, false},  {1024, 7, 12, 6, false},
    {1024, 6, 13, 6, false},  {1024, 5, 15, 7, false},  {1024, 4, 17, 8, false},
    {1024, 3, 19, 8, false},
    {512, 2, 40, 10, true},
    {128, 2, 85, 18, true},
};
constexpr int kPolicyCurvePoints = sizeof(kPolicyCurve) / sizeof(kPolicyCurve[0]);

// Seat revisit time in us: distinct frequencies (radio visits per sweep) x
// one visit at this capture length. No channels yet: assume eight.
inline int policyRevisitUs(int visits, int samples = kPolicySamples)
{
    if (visits < 1)
        visits = 8;
    return visits * (kPolicyVisitFixedUs + (samples * kPolicyVisitPerSampleNs + 500) / 1000);
}

// Shortest pass (whole ms) this point is meant for.
inline int policyBoundaryMs(const PolicyPoint& p, int visits)
{
    const int64_t num = (int64_t)p.readings * policyRevisitUs(visits, p.samples) * 10;
    const int64_t den = (int64_t)kPolicyReadingsPerRevisitX10 * 1000;
    return (int)((num + den - 1) / den);
}

// The least-jitter point whose boundary the pass reaches; past the end, the
// last (fastest) point.
inline int policyIndexFor(int passMs, int visits)
{
    for (int i = 0; i < kPolicyCurvePoints; ++i)
        if (passMs >= policyBoundaryMs(kPolicyCurve[i], visits))
            return i;
    return kPolicyCurvePoints - 1;
}

inline int policyWindowMs(const PolicyPoint& p, int visits)
{
    return (p.readings * policyRevisitUs(visits, p.samples) + 500) / 1000;
}

}  // namespace c5

#endif
