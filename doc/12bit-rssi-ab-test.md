# 12-bit RSSI A/B test plan

## Question being answered

Does widening the RSSI pipeline from 8-bit to 12-bit give usable extra
resolution, and do the per-node floor/peak values scale as predicted (x16)?

Measured on current 8-bit firmware (2026-09-21):

| node | floor | peak | span |
|------|-------|------|------|
| 1    |  91   | 189  |  98  |
| 2    |  95   | 179  |  84  |
| 3    |  70   | 167  |  97  |
| 4    | 116   | 203  |  87  |
| 5    |  95   | 205  | 110  |
| 6    | 109   | 208  |  99  |
| 7    |  88   | 194  | 106  |
| 8    |  89   | 190  | 101  |

Peak spread 41, floor spread 46, span spread 26. Max peak 208 vs clamp at 255
=> the old 0x01FF clamp never fired. Working span ~98 of 255 counts (38%).

Prediction: on 12-bit, every value multiplies by ~16.
floor 70 -> ~1120, peak 208 -> ~3328, span 98 -> ~1568.

## Toolchain (IMPORTANT)

Build with **STM32 core 2.12.0**, NOT 3.0.0.

Core 3.0.0 (GCC 14) produces a binary that flashes and verifies but never
enumerates - the node does not answer the revision-code handshake, and
because the serial-only bootloader entry needs running firmware to accept
JUMP_TO_BOOTLOADER, recovery then requires the physical BOOT0 button.
This happened on 2026-09-21 and cost a manual recovery.

The tell: a core-3.0.0 build is ~2.5KB smaller than stock and has NO
`FIRMWARE_VERSION:` / `FIRMWARE_PROCTYPE:` strings in the .bin. Always check:

    strings <file>.bin | grep FIRMWARE_

If that prints nothing, do not flash it.

## Recovery if a node fails to enumerate

Serial-only wiring means the Pi cannot reset the BPill. The software path
(JUMP_TO_BOOTLOADER over serial) needs firmware healthy enough to enumerate,
so it is unavailable exactly when it is needed. Recovery is the physical
BOOT0 button on the BlackPill:

  hold BOOT0 -> tap NRST -> release BOOT0

then flash immediately via /updatenodes. Note `/updatenodes` still works
with zero nodes detected - the server sets a mock serial object for this.

## Artifacts

On the Pi, `~/RotorHazard/firmware/`:

- `RH_S32_BPill_node_STM32F4.bin`       - current stock fw, untouched
- `RH_S32_BPill_node_STM32F4.bin.orig`  - backup of the above
- `RH_S32_BPill_node_STM32F4_v2_12bit.bin`    - TEST build (12-bit, API 37), core 2.12.0
- `RH_S32_BPill_node_STM32F4_v2_baseline.bin` - control, core 2.12.0

Server patch staged (NOT active) in `~/rh-12bit-staging/`:
`RHInterface.py`, `Node.py` (+ `.orig` copies).

Pi source was verified byte-identical to git HEAD before staging.

## Critical ordering

The node boots at API 37 and speaks 16-bit RSSI from the first packet.
An unpatched server misparses immediately. So: **server first, then flash.**
To revert: **restore firmware first, then server.**

## Procedure

### Phase 0 - record the control (optional, 10 min)

Current stock firmware is already running. Capture a reference so the
comparison is against today's conditions, not last week's numbers.

Repeat the same measurement used to produce the table above: note floor
(VTX fully powered OFF, not just disconnected) and peak (quad at a fixed,
marked position) for all 8 nodes.

Mark the quad position with tape. Phase 2 must reuse the exact same spot,
or the comparison is meaningless.

### Phase 1 - apply server patch

    cp ~/rh-12bit-staging/RHInterface.py ~/rh-12bit-staging/Node.py \
       ~/RotorHazard/src/interface/
    sudo systemctl restart rotorhazard

Requires a password and a TTY on this host:

    ssh -t rotorhazard@rotorhazard 'sudo systemctl restart rotorhazard'

Server is backward compatible - it still speaks 8-bit to API<=36 nodes, so
the timer keeps working on stock firmware after this step. Verify:

    grep "Serial multi-node found" ~/rh-data/logs/*.log | tail -1

Expect `API_level=36` still, and a working RSSI display. If RSSI reads zero
or garbage here, stop - the patch is wrong, revert before flashing.

### Phase 2a - flash the BASELINE first (toolchain validation)

Flash `RH_S32_BPill_node_STM32F4_v2_baseline.bin` BEFORE the 12-bit build.

It contains no behaviour changes, so it must behave exactly like stock:
`count=8, API_level=36`. This proves the core-2.12.0 toolchain produces a
working image before anything with real changes rides on it.

If this fails to enumerate, the toolchain is still wrong - recover via
BOOT0 and stop. Do not flash the 12-bit build.

### Phase 2b - flash 12-bit firmware

UI: Settings > Update Nodes. Select:

    /home/rotorhazard/RotorHazard/firmware/RH_S32_BPill_node_STM32F4_12bit.bin

Expect the version/build fields to read "(unknown)". This is a GCC 14
artifact (the unmodified baseline build shows it too), not a bad file. The
update button still enables.

After flash, confirm:

    grep "Serial multi-node found" ~/rh-data/logs/*.log | tail -1

Expect `API_level=37`, `count=8`.

If count < 8, the `MIN_RSSI_DETECT` threshold (raised 5 -> 80 for the new
scale) is mis-set and nodes are being seen as absent. That is a firmware
fix, not a wiring problem.

### Phase 3 - re-enter thresholds

All stored EnterAt/ExitAt are on the old 8-bit scale and are now ~16x too
low, so every node will sit permanently "crossing" until these are set.

Per-node EnterAt, computed as (floor + 0.6*span) * 16:

| node | 12-bit EnterAt |
|------|----------------|
| 1    | 2400 |
| 2    | 2320 |
| 3    | 2048 |
| 4    | 2688 |
| 5    | 2576 |
| 6    | 2688 |
| 7    | 2432 |
| 8    | 2400 |

Set ExitAt ~8% below EnterAt, matching the existing ratio.

### Phase 4 - measure

Repeat the Phase 0 measurement at the **same marked quad position**.
Record floor and peak for all 8 nodes.

For a continuous trace instead of eyeballed values, the server has a
built-in per-node CSV recorder. Set the env var before starting the
service (systemd unit, so it needs an override or a manual run):

    RH_RECORD_NODE_1=1 ... RH_RECORD_NODE_8=1

Writes `data_<n>.csv` in the server working directory, one row per update:

    readtime, lap_id, ms, current_rssi, node_peak, pass_peak, loop_time,
    cross_flag, pass_nadir, node_nadir, peakRssi, peakFirstTime,
    peakLastTime, nadirRssi, nadirFirstTime, nadirLastTime

## What the result means

**Success:** floors land near `floor*16` (node 3 ~1120, node 4 ~1856) and
peaks near `peak*16` (node 6 ~3328). Spans ~1568. Confirms the ADC was the
limit and 12-bit buys 4x finer steps across the working range.

**Partial:** values scale but land well below `x16` - the RX5808 output is
lower than the 10-bit read implied. Still a resolution gain, smaller than
hoped.

**Failure:** values do not scale, or are unstable/noisy. Then the limit is
analog (RX5808 saturation or front-end), and no ADC change helps. Revert
and pursue the analog path instead.

Also watch `loop_time` in the CSV or the Sensor Tuning panel. The 255-wide
running median has an early-out for repeated values:

    if (new_value == old_value) return;

Finer values collide less, so that early-out fires less often and the loop
gets slower. Compare loop_time before/after. A large rise means the median
needs replacing with a histogram-based O(1) implementation before this
firmware is race-ready.

## Revert (firmware first, then server)

1. Flash `RH_S32_BPill_node_STM32F4.bin.orig` via the update page.
2. Restore server:

       cp ~/rh-12bit-staging/RHInterface.py.orig \
          ~/RotorHazard/src/interface/RHInterface.py
       cp ~/rh-12bit-staging/Node.py.orig \
          ~/RotorHazard/src/interface/Node.py
       sudo systemctl restart rotorhazard

3. Re-enter the original 8-bit EnterAt/ExitAt values.

## Do not race on this build

Unvalidated crossing timing and an unmeasured loop-time cost. Bench only
until the numbers above are in and loop_time is confirmed acceptable.

## If the A/B succeeds - next step

Equalisation moves into firmware, applied once in `rssiRead()` so the
smoothing and crossing detection downstream stay untouched and fast:

    int adj = ((raw - floorOffset) * scaleFactor) >> 8;   // Q8 fixed point

Constants from the 2026-09-21 measurements, normalised to the median span
(98 counts). Offsets are 8-bit; multiply by 16 for the 12-bit build.
Scale factors are ratios and do not change.

| node | offset (8-bit) | offset (12-bit) | scale Q8 | scale |
|------|----------------|-----------------|----------|-------|
| 1    |  91 | 1456 | 257 | 1.005 |
| 2    |  95 | 1520 | 300 | 1.173 |
| 3    |  70 | 1120 | 260 | 1.015 |
| 4    | 116 | 1856 | 290 | 1.132 |
| 5    |  95 | 1520 | 229 | 0.895 |
| 6    | 109 | 1744 | 255 | 0.995 |
| 7    |  88 | 1408 | 238 | 0.929 |
| 8    |  89 | 1424 | 250 | 0.975 |

Applied, all 8 nodes land at span 98 (+/-0.2) - spread 41 -> 0.
Offset alone only gets spread 41 -> 26, which is why both constants matter.

Protocol work already scoped: opcodes 0x73/0x74 (write offset/scale),
0x34/0x35 (read), verified free. Plus `Profiles.floor_offsets` and
`Profiles.scale_factors` columns, transmitted at startup like EnterAt -
the node stores nothing across a power cycle.

Node 4 is worth a physical check first: highest floor (116) and narrowest
span (87), i.e. a noisy front end eating its dynamic range. Equalisation
would paper over a hardware fault. Node 3's low peak (167) is NOT a fault -
its span is 97, dead average; it just sits on the lowest floor.
