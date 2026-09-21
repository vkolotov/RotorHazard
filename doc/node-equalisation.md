# Per-node RSSI equalisation

Each RX5808 has a slightly different noise floor and gain, so the same quad
reads differently on every node. Equalisation measures both and applies a
per-node correction **in firmware**, so smoothing, peak/nadir tracking and
crossing detection all see corrected values.

    corrected = clamp( (raw - floorOffset) * scaleFactor >> 8 , 0, 4095 )

Q8 fixed point, integer only (no FPU use in the sample loop). Defaults
`floorOffset=0, scaleFactor=256` are the identity - no change to the reading.

Applied once in `RssiNode::rssiRead()`, which is the only place with the full
12-bit value. Downstream code is untouched.

## Why firmware and not the server

Crossing detection runs **on the node** (`rssiProcessValue` compares against
enterAt/exitAt). The server only receives already-decided passes. A
server-side correction would fix the graphs but not lap detection.

## Node API 38

| opcode | direction | payload | meaning |
|--------|-----------|---------|---------|
| `0x73` | write | int16 | floor offset |
| `0x74` | write | uint16 | scale factor (Q8) |
| `0x34` | read | int16 | floor offset |
| `0x35` | read | uint16 | scale factor (Q8) |

Nodes store nothing across a power cycle - the server re-sends both values at
startup via `Calibration.hardware_set_all_equalisation()`, the same way
EnterAt/ExitAt are handled.

## Database

Two columns on `profiles`: `floor_offsets`, `scale_factors` (JSON `{"v": [...]}`
like `enter_ats`). NULL reads as the identity values, so existing profiles keep
working untouched.

Run once against an existing database:

    cd ~/RotorHazard/src/server
    python3 util/add_equalisation_columns.py ~/rh-data/database.db

Idempotent, and safe to re-run.

## UI

Settings > Sensor Tuning. Each node gains **Floor** and **Scale** inputs below
EnterAt/ExitAt, both editable directly. Below the node list:

- **Reset Peak/Nadir** - clears the tracked extremes before a capture step
- **Equalise** - computes both constants from the captured extremes

## Procedure

1. Power the **VTX off completely** (not merely disconnected) and click
   **Reset Peak/Nadir**. Leave it off ~30 s so each node records its true
   noise floor as `nodeNadir`.
2. Power the quad on, hold it ~1 m in front of the sensor array, and step it
   through each node's channel in turn, pausing a few seconds on each, so every
   node records its `nodePeak`.
3. Click **Equalise**.

Floor offset is set to the measured nadir; scale factor normalises each node's
(peak - nadir) span to the **median** span across all nodes. Normalising to the
median rather than to full scale keeps every node inside its linear region -
the RX5808's RSSI output is a log detector and compresses near the top, so
stretching a node to full scale would amplify noise without recovering signal.

Values are written to the hardware immediately and persisted to the profile.

**EnterAt/ExitAt are not adjusted automatically.** After equalising, all nodes
share a common scale, so one EnterAt/ExitAt pair can be used for every node.

## Verified against measured data (2026-09-21)

Spans before: 690-858 (spread 168). Median 772.

| node | floor offset | scale Q8 | span after |
|------|--------------|----------|------------|
| 1 | 715 | 252 | 772 |
| 2 | 730 | 286 | 770 |
| 3 | 560 | 250 | 771 |
| 4 | 875 | 267 | 771 |
| 5 | 772 | 230 | 770 |
| 6 | 890 | 260 | 771 |
| 7 | 700 | 234 | 772 |
| 8 | 725 | 273 | 773 |

A quad at 60 % of each node's span reads **461-463 on every node** after
equalisation, versus 1034-1346 before.

## Caveats

- **Node 4** has the highest floor (875) and one of the narrowest spans - a
  noisy front end eating its dynamic range. Worth a physical check; equalisation
  will otherwise paper over a hardware fault.
- **Node 3 is not faulty** - its low readings are a low floor (560), and its
  span is average.
- Scaling up amplifies that node's noise by the same factor. Factors here span
  230-286 (0.90-1.12), so the effect is small.
