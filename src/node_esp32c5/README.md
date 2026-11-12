# RotorHazard node firmware for the ESP32-C5

Up to eight RotorHazard nodes from one ESP32-C5, with no RX5808 modules. The
C5's own 5 GHz Wi-Fi radio is the receiver: for each seat the firmware
retunes it, takes a raw I/Q capture, and measures the power in a band
around the pilot. That reading goes into **RotorHazard's own node core**
(`src/node/RssiNode.cpp` and `commands.cpp`). Crossing detection, history,
and peak/nadir tracking therefore follow upstream main. The C5 deliberately
uses upstream main's API 36 and standard 8-bit RSSI protocol; it does not
depend on the separate 12-bit RSSI or node-normalisation branches.

**Status (2026-11-10): builds with ESP-IDF 5.5.1 (`tools/build.sh`), all
`check_phy.py` checks pass; not yet run on hardware.** The portable parts
(meter, scanner, RH glue, RH's node core) pass host tests, including
RotorHazard's real `serial_node.discover()` and `RHInterface` over a
pseudo-terminal. The bench checklist below is the next step.

## Layout

| File | |
|---|---|
| `main/c5_radio.*` | Radio bring-up, PLL hop, gain, I/Q capture (ESP-IDF) |
| `main/c5_meter.*` | Band power from one capture: complex FIR band-pass, DC removal, optional image cancellation (portable) |
| `main/c5_scan.*` | Seat plan, LO side choice, out-of-band overload hold, dB to RH scale (portable) |
| `main/c5_rhglue.*` | RssiNode's receiver backend, the serial command link, firmware strings (portable) |
| `main/c5_main.cpp` | The node task, USB and UART links, console, settings in NVS (ESP-IDF) |
| `main/shim/` | The few Arduino calls RH's node core uses |
| `test/` | Host tests (`make -C test`) and the RH end-to-end test (`test_rh_e2e.py`) |
| `tools/check_phy.py` | Re-checks the PHY facts below against the libraries a build links |

Only two shared compile-time adaptations are required: `RHNODE_ESP32C5` makes
the processor a serial multi-node, and `RHNODE_RADIO_BACKEND` fences off the
RX5808 GPIO implementation that the C5 replaces. Both are false for existing
STM32 and AVR builds. No server/interface files, RSSI types, protocol commands,
normalisation code, or ADC-resolution code are changed.

## Build and flash

**ESP-IDF 5.5.1 exactly.** The PLL hop repeats the steps of that version's
`libphy.a`, and other PHY library versions use different ones (the copy in
Zephyr's `hal_espressif` calls `phy_set_freq_i2c_new` and `phy_restart_cal`
where 5.5.1 has `phy_set_freq_i2c` and `phy_wait_rfpll_cal_end`).
`check_phy.py` fails if the linked library differs.

```text
cd src/node_esp32c5
./tools/build.sh c5zero          # or xiaoc5; builds into build-<board>, then runs check_phy.py
. ~/esp/esp-idf/export.sh && idf.py -B build-c5zero -p /dev/ttyACM0 flash
```

All static data must end below the capture bank at 0x40820000; RH's eight
nodes take ~14 KB of BSS, so FreeRTOS and ring-buffer code are placed in
flash (`sdkconfig.defaults`). `check_phy.py` keeps a 2 KB margin.

For an RS422 or serial-radio link instead of USB, add
`-DRHNODE_C5_LINK_TX=<gpio> -DRHNODE_C5_LINK_RX=<gpio>`. The link runs at
921600 8N1, the S32_BPill's rate.

## Connecting to RotorHazard

Use a USB data cable to the C5's native USB port. Add the port (preferably a
`/dev/serial/by-id/...` path) to `SERIAL_PORTS` and restart RotorHazard. It
finds eight standard 8-bit nodes. Configure thresholds in RotorHazard as for
other API-36 nodes.

The firmware sends nothing until asked. Opening the port resets the C5, and
RotorHazard waits 2 s before its first request, which boot has to fit in.
Logs go to UART0 only.

## Identity and name (1.2.0-c5.13)

A node is a timer in mach1, however it is reached (a laptop over USB or the
AirPort link, or a C5 relay), so it says who it is:

- `READ 0x43`: its id, 6 bytes, the chip's factory MAC (read-only).
- `READ 0x44` / `WRITE 0x75`: its name, 32 bytes of UTF-8, zero padded,
  kept in NVS (`rhc5`/`name`). A new board answers "C5 " and the end of its
  MAC, e.g. `C5 26A8`.
- Feature flag `0x0080` (`RHFEAT_C5_NODE_ID`) says it has both.

mach1 knows the node as `c5-<id>`; renaming it in mach1 or on its relay
writes the name here. Variants in use: `build-c5zero-node` (USB only) and
`build-c5zero-node-link` (AirPort UART link, TX 11 / RX 12); a board must be
flashed with the variant it runs, or it loses its link. See mach1's
`doc/c5-timers-field-guide.md` for the boards and flashing through a laptop.

## Console

On the USB port, start a line with `!` (RotorHazard never sends 0x21). On
UART0, any line works. `!help` lists the commands:

| Command | |
|---|---|
| `status` | Version, radio bring-up checks, gain, capture counters, settings |
| `scan [cycles]` | Per frequency: mean and max band power, whole-capture power, clipped samples, image estimate |
| `rf <MHz>` | With a VTX on MHz: pilot and mirror readings with the LO on each side (image rejection) |
| `freq <seat> <MHz>` | Tune a seat without RotorHazard |
| `gain N`, `n N`, `meter taps hop`, `image off\|cancel`, `map floorDb` (4 RSSI counts per dB, fixed), `hold clipMin oobDb`, `skip mask`, `stock 0\|1`, `seats N` | Settings, kept in NVS |
| `autosave 0\|1` | Gate the normal two-second NVS autosave. Always returns to `1` at boot; use `0` for runtime-only experiments and explicit `save` to persist. |
| `stats`, `save`, `reboot` | |

## How a reading is made

1. **LO 10 MHz off the pilot**, PLL-only retune (`hop()`, about 70 µs
   instead of the stock 400 µs).
2. **Capture**: 256 samples at 80 MS/s (3.2 µs), triggered directly through
   the capture engine's registers (about 10 µs; the stock call costs 1.7 ms).
3. **Meter**: a 32-tap Kaiser-windowed band-pass (cutoff 7.5 MHz, its −6 dB
   point) centred on the pilot, evaluated every 8 samples, after removing
   the capture's DC. Its mean output power in dB is the reading. It is within
   1 dB to ±4.5 MHz, −20 dB at 10 MHz, −66 dB at 15 MHz and −92 dB at 37 MHz.
4. **Scale**: four reported RSSI counts per dB. Internally `map` stores that
   as Q4, so its default 64 means 4 counts/dB. The default floor is −8 dB.
5. Into `RssiNode::rssiProcessValue()`, timestamped at the capture's middle
   on the same clock RH reads with `READ_TIME_MILLIS`.

## Ghosting: one VTX showing on another seat

All seats share one radio, so a VTX can leak into another seat's reading
along four paths. Each has its own defence:

| Path | Defence | Expected (model or bench) |
|---|---|---|
| **Neighbour** in the meter's band | The FIR's selectivity | −66 dB at 15 MHz, −92 dB at 37 MHz (design). With a flat front end, a 49.5 dB VTX 20 or 37 MHz away moves a seat by under 0.3 dB (host test) |
| **Image**: I/Q imbalance mirrors the other side of the LO onto the pilot | LO side chosen per seat so the mirror is empty; optional blind image cancellation (`image cancel`) | About 18 dB rejection on chip v1.0 at gain 30 (published bench figure). Cancellation recovers 31 dB in simulation |
| **Alias**: 80 MS/s sampling folds a VTX 80 MHz from where the pilot lands onto it | Only the analogue front end stops it. The LO side is chosen to minimise the worst expected leak | On Raceband, two channels away (64 or 84 MHz from the LO) gives −46 to −58 dB on a 5th-order model of the front end. **Unmeasured beyond ±38 MHz** |
| **Overload**: a strong VTX elsewhere clips the ADC and smears across the band | A capture that clips while the pilot band holds little of its power is not used; the seat keeps its last good reading | Host test: a 20 MHz neighbour driving full clipping leaves the seat unchanged |

The LO side choice weighs all of these against the frequencies actually in
use (`Scanner::leakDb()`). For Raceband it uses LO below for R1–R6 and R8,
and LO above for R7.

## Bench checklist

In order. Each item says what to expect; a different result is a finding.

1. **Build** passes `tools/check_phy.py` (PHY facts, `adctrig` linked,
   static data below 0x40820000).
2. **Bring-up**: `!status` shows `wifi=1 live=1`, `gainmax` about 89 on chip
   v1.0, and no captures timing out or incomplete.
3. **Direct against stock capture**: `!stock 1`, then `!scan`, then
   `!stock 0`, then `!scan` with a steady VTX. Means within about 0.2 dB.
   If direct captures come back incomplete, the count field may want N−1;
   see `directCapture()`.
4. **Spectrum orientation**: `!rf <VTX MHz>`. The pilot reads high and the
   mirror 15–35 dB lower on both sides. If the mirror reads high, flip
   `kSpectrumSign`.
5. **Noise floor**: `!scan` with no VTX should read steady, a few dB above
   −8 dB, on every Raceband channel.
6. **Neighbours**: VTX at race power, about 1 m away, on R4. In `!scan`, R3
   and R5 should stay within about 1 dB of their floor.
7. **Alias**: same VTX on R2, then R6, watching R4. The model predicts at most
   a few dB at close range; anything near EnterAt needs a stronger LO-side
   rule or crosstalk subtraction.
8. **Image**: VTX on 5780 and seat on 5800 (LO below puts its mirror on
   5780). Compare `image off` and `image cancel`.
9. **Overload**: VTX within 20 cm. `!stats` shows held readings, and the
   other seats do not count laps.
10. **RotorHazard**: discovery as eight API-36, 8-bit nodes, then controlled
    slow and fast passes against the S32_BPill.

## Where the radio facts come from

The C5's capture path is undocumented. Sources: **D**, our own disassembly
of Espressif's Apache-2.0 `libphy.a` for the ESP32-C5 (the copy in Zephyr's
`hal_espressif`, 2026-11-05; `tools/check_phy.py` repeats the checks on the
library a build links). **P**, facts published in FPVGate C5MK's
documentation (`RADIO_INTERNALS.md`, `RF_RESEARCH.md`, `PROVENANCE.md`).
**E**, ESP-IDF. **M**, bench measurement, still to do. No source code from
FPVGate C5MK (CC BY-NC-SA) or the GPL projects (esp-sdr, C5VRX, Widscan58,
double-ESP-resso, LapDog) was used. Two C5MK headers (`radio.h`, `dump.h`)
were read for orientation before the decision to reimplement; nothing was
copied from them. The meter, scanner, ghost defences and RH integration are
this project's own design.

| Fact | Source |
|---|---|
| `phy_set_rfpll_freq(xtal, MHz, trim, sdm*)` stores the MHz at `phy_param+714`, then (ESP-IDF 5.5.1) calls `phy_rfpll_set_freq`, `phy_write_rfpll_sdm`, `phy_set_freq_i2c(MHz)`, `phy_wait_rfpll_cal_end(MHz)` (restarts the VCO calibration and polls it, 20 µs per try), `phy_i2c_sdm_en(phy_param[42])` and ends with `ets_delay_us(300)` | D |
| `phy_set_rf_freq_offset` follows that with `phy_ckgen_5g_cal(MHz)` (5 GHz) and `phy_freq_mem_change_5g(sdm*)`, with the SDM state at most 12 bytes on its stack; in 5.5.1 it skips the PLL step when `phy_param[1006]` is clear (`hop()` always does it) | D |
| Espressif's channel code passes `phy_param[49]` (xtal) and the int16 at `phy_param+30` (trim) | D |
| `phy_set_chanfreq(MHz, bw)` sets the 5 GHz flag (`phy_param[42]`) and calls `phy_chip_set_chan` | D |
| `phy_force_rx_gain(enable, index)`: index to bits 24–31, enable to bit 23 of `0x600A702C` | D |
| Readings settle without the 300 µs wait | P |
| Re-forcing a gain that already holds disturbs the receiver | P |
| Highest calibrated gain index in bits 8–14 of `0x600A702C` | P |
| `adctrig` (in `librftest.a`, linked with `CONFIG_ESP_PHY_ENABLE_CERT_TEST`) takes 9 arguments (the 9th is read from the caller's stack); arg 2 selector, arg 3 mode, arg 6 wait style. Mode 1 runs `phy_start_tx_tone_step`; `trig_tx_frame` runs only for selector 5 or 6 (it tests selector − 5 ≤ 1) or mode 2; selector 12 starts BLE. Selector 0, mode 0, wait style 0 reaches none of them | P, confirmed D (ESP-IDF 5.5.1 `librftest.a`) |
| Capture engine registers: control `0x600A9004` (count 0–16, done 18, trigger 19, enable 31), SRAM ownership `0x60095004` (bits 8–11 = 2, bit 16), samples from `0x40830000`, modem owns the bank `0x40820000`–`0x4083FFFF` | P |
| Sample word: bits 0–9 signed I, 10–19 signed Q, 20–26 gain index; RF above the LO appears at negative frequency | P |
| Wi-Fi in NULL mode with promiscuous receive keeps the PHY powered for captures; `CONFIG_ESP_PHY_DISABLE_PLL_TRACK` stops the PLL being retuned underneath | P, E |
| Front-end passband about ±20 MHz, at the floor by ±36 MHz; image rejection about 18 dB at gain 30 | P |
| Supported pilot range 5180–5917 MHz | P |


## Native-cadence diagnostics, short mean and profile gates (1.2.0-c5.4-profile-gates, 2026-11-10)

`autosave 0` makes console experiments volatile. It does not change or erase
the saved NVS profile, explicit `save` still works, and every reboot restores
the normal `autosave 1` behavior. `status` reports both autosave and dirty state.

`!record arm <seat> <count>` records observations from the normal scanner loop
without serial output or allocation during acquisition. `!record status`
reports capacity and completion; `!record stop` ends acquisition early.
After stopping, `!record read <offset> <count>` returns up to 16 indexed rows.
The buffer is allocated once at boot (1024 records on the tested C5 Zero,
with smaller fallback capacities); check capacity before arming.

Rows are `r index tUs bandDbQ8 wideDbQ8 clipped flags goodAgeUs`.
Flags: 1 failed, 2 held, 4 clipped. Failed power values are placeholders, not
measurements. Age is UINT32_MAX before a good sample. Use unsigned timestamp
differences across rollover. Recording is observational: detector handling of
held values is observable. The legacy `trace` allocation is too large for the
current node heap; use the bounded recorder.

The live detector now defaults to `filter mean 2`. It averages two readings in
linear power before RSSI mapping and timestamps the output at the
stronger raw reading. A failed or overload-held capture clears the short input
window and is not submitted as a fresh sample. `filter mean <1..5>` selects a
short mean; `filter median <ms>` retains the time-sized RotorHazard median as a
rollback. Legacy `filter <ms>` also selects the median. Mode and length persist
in NVS. Changing mode resets the filters; do it outside an active race.

Host checks: **151 passing**, including fixed-point power-mean value, peak
timestamp, held reset, pass-through and existing RH detector behavior.

Simulator 0.6-diag adds `prepare <MHz>`: start the PLL, then mute both tone
generators before receiver acquisition. Configure `quad`, call `prepare`, wait
for settling, arm the receiver, then call `go`. Matching prepared passes reuse
the PLL and finish muted; `off` stops the source. Legacy unprepared `go` still
starts its PLL during the test and may emit a startup transient. Prepared mode
was added after raw traces exposed that artifact. It is not a more accurate
physical antenna model: the envelope still updates approximately every 250 us.

Bench scripts and results live in the sibling laptimer repository under
`analysis/esp32c5-20261110-diag/` and
`docs/esp32c5-contiguous-experiments-20261110.md`. That report contains the
offline comparison, live detector matrix and capture/meter loop tradeoffs. The
250 ms median is unsuitable for the tested narrow horn passes. The saved bench
baseline remains 256 samples, 32 taps and meter hop 8 until reduced settings
are validated with real modulated VTXs.
