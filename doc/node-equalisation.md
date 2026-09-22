# Per-node RSSI equalisation

Receivers differ in sensitivity, so the same quad reads differently on every
node and each one needs its own EnterAt/ExitAt. Equalisation measures three
signal levels per node and corrects the reading **on the node itself**, so one
threshold pair works across the whole fleet.

Requires node API 37 (12-bit RSSI + piecewise correction).

## What it corrects, and what it cannot

Two goals, both measurable:

1. **Same reading for the same power.** One quad stepped through every channel
   should peak at the same value on every node.
2. **Same response between two powers.** Going from PIT to race power should
   move every node by the same amount, so quads approaching from a distance
   read alike.

Goal 2 is the hard one. The RX5808's RSSI output is a **log detector**, and
nodes sit at different points on that curve because their front-end gain
differs - the ratio (pit-floor)/(race-pit) varies 4.8x across an 8-node fleet.
A single gain factor pins whatever two levels it was fitted to and diverges
everywhere else, which is why an earlier floor+scale design looked correct at
the peak and was 57 % out at PIT.

## The correction

Two straight segments meeting at the PIT level:

    raw >= pivot:  corrected = 300 + ((raw - pivot) * kUp) >> 8
    raw <  pivot:  corrected = 300 - ((pivot - raw) * kLo) >> 8
    clamped to 0 .. 4095

- `pivot` is the raw ADC reading at PIT power
- `kUp` maps PIT->300 and race->800. **This is the accurate segment** and the
  range crossing detection operates in.
- `kLo` maps the noise floor to ~30. This one is deliberately rough.

Q8 fixed point throughout, no floating point in the sample loop. `pivot = 0`
disables the piecewise path entirely.

### Why the lower segment exists at all

The noise floor carries nothing useful for lap detection, so it is not worth
fitting accurately. But a node whose corrected floor sits at 0 shows a flat
line with no jitter, which reads as a dead node. Landing it near 30 keeps the
idle trace visibly alive - 1.3 to 3.0 counts of movement - at no cost to the
segment that matters.

Targeting 0 would also clip: with sigma around 3 counts, the lower half of the
noise distribution would hit the clamp, biasing the mean up and destroying the
symmetry the median filter assumes.

### Applied once, at the source

The correction goes in `RssiNode::rssiRead()`, the only place with the full
12-bit value. Everything downstream - smoothing, peak/nadir tracking, crossing
detection - sees corrected values with no further change.

This has to be in firmware. Crossing detection runs on the node against
enterAt/exitAt; the server only receives decided passes. A server-side
correction would fix the graphs and leave lap detection uncorrected.

## Node API 37

| opcode | direction | payload | meaning |
|--------|-----------|---------|---------|
| `0x66` | write | uint16 | pivot (raw ADC at PIT) |
| `0x67` | write | uint16 | kUp (Q8) |
| `0x68` | write | uint16 | kLo (Q8) |
| `0x36` | read | uint16 | pivot |
| `0x37` | read | uint16 | kUp |
| `0x38` | read | uint16 | kLo |
| `0x77` | write | - | reset peak/nadir tracking |

Nodes keep nothing across a power cycle, so the server re-sends all three at
startup and on profile switch, exactly as it does for EnterAt/ExitAt.

## Database

Three columns on `profiles`: `eq_pivots`, `eq_kups`, `eq_klos`, JSON
`{"v": [...]}` like `enter_ats`. NULL reads as "no correction", so existing
profiles keep working untouched.

    cd ~/RotorHazard/src/server
    python3 util/add_eq_piecewise_columns.py ~/rh-data/database.db

Idempotent, safe to re-run.

## Running a calibration

Settings > Sensor Tuning, above Calibration Mode. One button that relabels
itself through the sequence, with **Reset** and **Back** alongside.

    Capture: noise -> Capture: R1 low -> Capture: R1 high
                   -> Capture: R2 low -> ... -> Apply -> Equalised

17 steps for 8 channels: one noise capture plus a low/high pair per channel.
The per-node readout under each graph fills in as you go, so a node that never
saw the quad is obvious before you apply.

**Keep the quad in one fixed position throughout.** Every capture is compared
against the others, so moving it looks like a sensitivity difference.

1. **noise** - VTX powered off completely. No quad needed, no channel changes.
2. **low** - VTX in PIT mode, on the channel being captured.
3. **high** - VTX at race power, same channel, same position.
4. Repeat 2-3 for each channel, then **Apply**.

Each click resets the trackers, waits `EQ_SETTLE_SECONDS` (5 s), then reads.
The button disables and counts down while it measures.

### Why the reset happens after the click

A node peak only ever rises. If the trackers were cleared at the *end* of the
previous step they would already hold whatever the VTX was doing while the
channel was being changed - typically still at race power - and a later PIT
reading could never pull them back down. Resetting after the click means the
captured value can only come from the condition set up right now.

This matters in practice: with a PIT switch on the radio and channel changes
through VTX Admin, the VTX passes through race power on the new channel before
the operator flips back to PIT. Without the late reset, every `low` capture
after the first would silently record the `high` value.

### Reset clears everything

**Reset** drops the applied constants as well as any part-finished capture, and
pushes identity values to the nodes. It has to: captures are only meaningful
against uncorrected readings, so starting a new run while a correction is live
would measure corrected values and fit nonsense.

Once applied, the button reads **Equalised** and is disabled. Reset is the only
way to start again, so a stray click cannot overwrite a good calibration.

## Validation

`Apply` refuses if any node's levels are too close together
(`MIN_LEVEL_SEPARATION = 60` raw counts between adjacent levels), naming the
node. A node that never saw the quad reads only noise and would otherwise get
an absurd slope - a span of 5 counts becomes a 156x gain.

60 is deliberately loose. A node sitting high on its detector curve compresses
legitimately: in the measured fleet node 4's PIT->race step is only 188 counts
while node 5's is 497. An earlier threshold of 200 rejected node 4's perfectly
good capture.

## Measured results

Eight nodes, each on its own channel, quad at 1 m:

| | before | after |
|---|---|---|
| race spread | 21.4 % | **0.13 %** |
| PIT spread | 43.1 % | **0.00 %** |
| idle floor | 558-909 raw | 30-31, sigma 1.3-3.0 |

## Constraints worth knowing

**Nodes must stay on the channel they were calibrated on.** Constants fitted on
one channel do not transfer: applied band-wide they take cross-node spread from
21.3 % to 20.0 %, and make five of eight channels *worse* than no correction at
all. Front-end sensitivity varies per node across the band - some nodes are
flat to 6 %, others vary 24 % and not in the same direction. See
`rssi-channel-survey.md` for the measurements.

The normal fixed assignment (node 1 on R1, node 2 on R2, ...) is the supported
configuration. Reassigning channels between heats needs a fresh calibration.

**PIT mode still transmits.** It is a low-power mode, not off. The noise capture
needs the VTX genuinely powered down, or the "floor" records signal and the
whole fit shifts.

**Between the fitted points the correction is approximate.** Two segments pin
three levels exactly; the log curve between them is not modelled. A third real
power level would allow a better fit, but the region that matters for crossing
detection sits between PIT and race, where the accurate segment already runs.

## Legacy affine path

An earlier design used a single `floor_offset` + `scale_factor` per node
(`(raw - floor) * scale >> 8`). It is still present in the firmware as the
fallback when `pivot == 0`, and the server still carries `calibrate_floor`,
`calibrate_scale` and `equalise_nodes` with live socket handlers, though the UI
no longer exposes buttons for them.

It is superseded. Fitting the noise floor and race level pins those two points
and leaves the middle of the range wrong - measured 57 % PIT spread against
0.2 % at the peak - because the detector is logarithmic and the noise floor is
not a point on the signal response curve at all. The `floor_offsets` and
`scale_factors` columns remain for the same reason.

Setting a piecewise `pivot` takes precedence; the affine constants are then
ignored by the firmware.

## Related

- `rssi-channel-survey.md` - why constants are channel-specific, with data
- `rssi-channel-survey.csv` - the raw 192 measurements
- `12bit-rssi-ab-test.md` - the 12-bit pipeline this builds on
- `12bit-measurements.md` - what that change actually bought, measured
- `src/server/rssi_survey.py` - the capture tool used for the survey
