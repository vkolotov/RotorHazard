# Proposed RSSI normalisation: gate-anchored, offset-only at the gate

Status: proposal. Supersedes the fit currently implemented in
`src/server/calibration.py` (`_eq_destination` and the two-slope fit).

## What is wrong with the current fit

The shipped fit maps each node's captured `low` and `high` onto a shared
destination derived from the widest span in the fleet. It achieves cross-node
agreement, but it reaches it by applying a **gain** through the whole curve,
including the strong-signal region where the gate crossing is decided.

Measured on the eight-node fleet (`rssi-channel-survey.csv`, channel R3), a
20-count fluctuation in the raw reading at the gate comes out as:

| node | raw ±10 at gate | current output |
|------|-----------------|----------------|
| 1    | 20              | 57 (2.85x)     |
| 2    | 20              | 45 (2.25x)     |
| 3    | 20              | 24 (1.20x)     |
| 4    | 20              | 46 (2.30x)     |
| 5    | 20              | 20 (1.00x)     |
| 6    | 20              | 23 (1.15x)     |
| 7    | 20              | 33 (1.65x)     |
| 8    | 20              | 26 (1.30x)     |

Every node's peak shape is distorted by a different factor. Peak amplitude,
local slope and the timing of the RSSI maximum are all scaled, which is
exactly what the crossing algorithm reads.

## The proposal

Two measurements per node, everything else derived:

    FLOOR_RAW   RSSI with the VTX off, median over several seconds
    HIGH_RAW    RSSI with the quad at the gate at calibration power

System-wide parameters:

    TARGET_HIGH     where the gate should land, as a fraction of full scale
    TARGET_FLOOR    where the floor should land
    PIVOT_RATIO     0.60 - where gain correction gives way to offset only

The transfer function is two straight segments meeting at a per-node pivot:

                           offset only, gain = 1
                                      /
                                     /
    TARGET_HIGH  ----------------- HIGH
                                   /
                                  /
    PIVOT_TARGET ------------- PIVOT
                              /
                            /  gain corrected
                          /
    TARGET_FLOOR ----- FLOOR

    HIGH_OFFSET  = TARGET_HIGH - HIGH_RAW
    PIVOT_RAW    = FLOOR_RAW + (HIGH_RAW - FLOOR_RAW) * PIVOT_RATIO
    PIVOT_TARGET = PIVOT_RAW + HIGH_OFFSET
    LOW_GAIN     = (PIVOT_TARGET - TARGET_FLOOR) / (PIVOT_RAW - FLOOR_RAW)

    if raw >= PIVOT_RAW:  out = raw + HIGH_OFFSET
    else:                 out = PIVOT_TARGET + (raw - PIVOT_RAW) * LOW_GAIN

Anchoring the lower segment at the pivot rather than at the floor is what
makes the two segments meet.

The pivot is **per node**, computed from that node's own floor-to-gate range.
There is no fleet-wide raw pivot.

## Why the gate region carries no gain

For `raw >= PIVOT_RAW` the derivative is exactly 1, so:

- peak amplitude is preserved
- local slope is preserved
- the timing of the maximum is unchanged
- a raw fluctuation of n counts is n counts on the output

The upper region is where a pass is decided, so it is left undistorted. Gain
correction is applied only further down, where aligning receiver sensitivity
matters more than preserving shape.

## Measured results

Emulated against `rssi-channel-survey.csv` - 8 nodes x 8 R-band channels,
each level a median of ~80 samples.

Cross-node spread at the gate, calibrated on each channel in turn:

| channel | uncorrected | proposed | current |
|---------|-------------|----------|---------|
| R1      | 26.7%       | 0.00%    | 0.19%   |
| R2      | 19.8%       | 0.00%    | 0.28%   |
| R3      | 18.6%       | 0.00%    | 0.18%   |
| R4      | 18.6%       | 0.00%    | 0.10%   |
| R5      | 20.5%       | 0.00%    | 0.19%   |
| R6      | 19.5%       | 0.00%    | 0.19%   |
| R7      | 20.3%       | 0.00%    | 0.38%   |
| R8      | 22.4%       | 0.00%    | 0.28%   |

Exactly zero, because the gate maps by pure offset.

Cross-node spread at a fixed fraction of each node's own floor-to-gate range,
averaged over all eight channels. This is the useful metric: it asks whether
two nodes seeing the same relative signal report the same number.

| fraction of range | uncorrected | offset only | proposed (0.60) |
|-------------------|-------------|-------------|-----------------|
| 0.3               | 32.4%       | 11.0%       | 4.0%            |
| 0.4               | 30.0%       | 8.8%        | 4.5%            |
| 0.5               | 27.9%       | 6.9%        | 4.9%            |
| 0.6               | 26.1%       | 5.2%        | 5.2%            |
| 0.8               | 23.0%       | 2.3%        | 2.3%            |
| 1.0 (gate)        | 20.8%       | 0.0%        | 0.0%            |

The gain segment earns its place below 0.6 - it roughly halves the error at
0.3 to 0.5 against offset-only - and costs nothing above it.

`PIVOT_RATIO` was swept over 0.0, 0.3, 0.6, 0.8 and 1.0. Gate agreement is
0.00% at every value, so the ratio only trades convergence in the lower
region; 0.60 measured best over 0.3-0.5.

### Do not judge this by the PIT level

The survey's `pit` level is a transmit-power setting, not a point on the
curve: across the fleet it lands anywhere from 28% to 80% of the
floor-to-gate span. Spread measured at `pit` therefore reports where each
node's PIT happens to fall, not normalisation quality. Use a fixed fraction
of each node's own range instead.

## Behaviour across frequencies

Calibrated on R1, then the fleet moved to other channels:

| channel | gate spread | fleet mean vs target |
|---------|-------------|----------------------|
| R1      | 0.0%        | +0.0%                |
| R2      | 6.3%        | +0.9%                |
| R3      | 9.0%        | +0.9%                |
| R4      | 8.3%        | +0.7%                |
| R5      | 6.6%        | +1.5%                |
| R6      | 8.4%        | +2.5%                |
| R7      | 11.1%       | +2.6%                |
| R8      | 6.0%        | +2.3%                |

Better than the current fit, which reaches 9-20% on the same test, because
the offset region does not amplify the drift. The absolute level holds within
about 3% of target across the band, so a threshold set on one channel stays
roughly right on the others; cross-node agreement is what degrades.

A fit is therefore channel-specific. Storing one set of constants per
frequency would restore 0.00% on every channel, and is cheaper under this
design than the current one: one `HIGH_OFFSET` per node per frequency, with
the pivot and gain derived from it.

**This is measured on R band only - 259 MHz.** A mixed-band event, for
example L1 L3 L5 L7 with R1 R3 R5 R8, spans 555 MHz. Since cross-node
agreement already degrades to 11% within one band, expect worse across two.
There are no L-band measurements yet; a sweep is needed before designing
per-frequency storage.

## Implementation notes

Integer only, no floating point in the sample path. Compute the constants
once when calibration is loaded:

```cpp
struct RssiCalibration {
    uint16_t floorRaw;
    uint16_t highRaw;
    uint16_t pivotRaw;
    uint16_t pivotTarget;
    int16_t  highOffset;
    uint16_t lowGain_x256;
};

static inline rssi_t normaliseRssi(uint16_t raw, const RssiCalibration& c)
{
    int32_t value;
    if (raw >= c.pivotRaw) {
        value = (int32_t)raw + c.highOffset;          // gate region, gain 1
    } else {
        int32_t delta = (int32_t)raw - c.pivotRaw;
        value = (int32_t)c.pivotTarget + ((delta * c.lowGain_x256) >> 8);
    }
    if (value < 0) return 0;
    if (value > MAX_EQ_RSSI) return MAX_EQ_RSSI;
    return (rssi_t)value;
}
```

Apply it in `rssiRead()`, the single point where a reading enters the
pipeline, so smoothing, peak tracking and crossing all see normalised values.

Two details that the measurements turned up:

- **Continuity at the pivot is approximate.** With integer truncation the
  step across the pivot measured 2 to 3 counts over all 64 node/channel
  fits. That is inside the post-filter noise (sigma about 3.5 counts at 12
  bits) and harmless, but the mapping is continuous to within one count of
  `LOW_GAIN`, not exactly continuous.

- **Targets must be fractions of scale, not constants.** `TARGET_HIGH = 200`
  is a sensible 78% of an 8-bit range and a pointless 5% of a 12-bit one.
  Express both targets as a fraction of what the node's pipeline can carry,
  and keep the gate well below full scale - a quad closer than the
  calibration point reads higher and must not clip. The emulation used
  TARGET_HIGH at 39% of scale, leaving about 2.5x headroom.

## Calibration procedure

Per node, two captures:

1. Let the receivers warm up.
2. VTX off. Sample for several seconds, take the median as `FLOOR_RAW`.
3. Quad at the marked gate position, at the chosen calibration power.
4. Sample for several seconds, take the median as `HIGH_RAW`.
5. Derive offset, pivot and lower gain; store.

Two captures instead of three shortens the wizard from 17 steps to about 9
for an eight-node fleet, and removes the mid-power capture that was the
least reproducible of the three.

## Validation

With the same VTX and the same physical positions, compare all nodes at: the
gate, just before and after it, mid distance, far, and VTX off. Expect gate
readings to converge on `TARGET_HIGH`, fluctuations near the gate to keep
their raw amplitude, the region above the pivot to be a pure translation of
the raw curve, readings below the pivot to converge progressively, and no
visible step at the pivot.
