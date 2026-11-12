# RotorHazard relay for the ESP32-C5

A C5 board that drives a RotorHazard node over a UART (the 900 MHz AirPort
link to the gate, or a wire) and **is the timer**: it runs the race and
speaks mach1's timer protocol over Wi-Fi. No Raspberry Pi, no RotorHazard
server, no laptop or Chrome tab.

```
gate:   C5 node ─UART 921600─ SuperX Nano ≈≈ 900 MHz AirPort ≈≈
relay:  XR4 ─UART 921600─ C5-Zero relay ── Wi-Fi ── mach1 (wss)
bench:  C5 node TX/RX pads ─wire─ C5-Zero relay TX/RX pads
```

Designs: laptimer `docs/standalone-gate-relay-design.md` (hardware, AirPort,
FPVTrackside) and `docs/browser-relay-design.md` (the mach1 protocol).

## What it does

- **Node driver** (`main/core/node_driver.*`): RotorHazard's start-up and
  seat sweep, ported from mach1's browser timer (`nodeDriver.ts`). A new lap
  id counts only once a second read agrees (garbage replies pass the
  checksum). Crossing time = request send time + half the smallest round trip
  − the node's "ms since crossing". 50 ms timeout, 4 retries.
- **Race engine** (`race_engine.*`): holeshot, minimum lap, time limit and
  grace, first to X, results, as `raceEngine.ts`.
- **mach1 timer** (`mach1_timer.*`): the `rh_mach1` plugin's protocol:
  `hello`/`status` heartbeat, `rh.*` race messages kept until acked and
  resent after a reconnect, commands `start/stop/discard/set_levels/
  set_channel`, `gate_config` (preferred channels, default and pilot levels,
  applied at connect and 2 s after a race), and `watch` → `nodes` frames for
  mission control's live node view. It names its gate by the node's
  processor (`hardware: "ESP32C5"`) like the browser timer, so both share
  the levels saved in mach1. Because its source is not `browser`, mission
  control treats it like a RotorHazard timer (saves are pushed to it).
- `relay_main.cpp` (ESP-IDF): UART1 link, Wi-Fi STA, SNTP, WebSocket client
  (TLS with the CA bundle), USB console or serial pass-through, settings
  in NVS.

Not yet: the FPVTrackside Socket.IO server (phase 2), OSD via a Timer
Backpack, keeping unacked race messages across a reboot (RAM only, 400 max).

## Build and flash

```text
cd src/relay_esp32c5
. ~/esp/esp-idf/export.sh
idf.py -B build set-target esp32c5      # once
idf.py -B build build                    # link on GPIO11/12 by default
idf.py -B build -DRELAY_LINK_TX=<gpio> -DRELAY_LINK_RX=<gpio> build
idf.py -B build -p /dev/serial/by-id/<relay> flash
```

No PHY hacks, so any ESP-IDF 5.5.x works; `esp_websocket_client` comes from
the component registry.

## Set up (USB console, 115200 is irrelevant: native USB)

```text
wifi <ssid> <password>
feed https://<mach1 host>/<club>/feed/relay/?key=<club feed key>
name Club relay
reboot
status
```

The `feed` URL is the club feed URL with the source segment of your choice
(`relay`); the relay turns it into `wss://<host>/ws/timers/<club>/relay/`
with its own uid (`relay-<mac>`). `minlap <ms>` sets the minimum lap,
`log` shows recent events. A start needs the wall clock (SNTP) and a
running node; otherwise mach1 gets a failed `command_result` saying why.

## Wi-Fi or serial (one USB port)

The relay is a timer on its own over Wi-Fi. Plug it into a laptop instead
and mach1's **browser timer** can drive the gate through it over Web Serial
(pick the relay's port; it is the C5's native USB, `303a:1001`):

- A line starting with a letter is a console command. Anything else is the
  node protocol (RotorHazard never starts with a letter: the browser opens
  with `READ_REVISION_CODE` 0x22, and C5 node settings are `!` lines), and
  the relay hands the host its node link: bytes are forwarded both ways
  between USB and the radio UART, its own polling stops (mach1 sees the
  gate down) and its logs are silenced so they cannot corrupt the stream.
- After 3 s without a byte from the host (browser stopped, cable out) the
  relay takes the node back and reconnects (`Serial ended` in `log`).
- Wi-Fi and the mach1 connection stay up throughout.
- Opening the port resets the C5 (USB-serial DTR/RTS); the browser's
  start-up retries cover the reboot. Its leftover boot text is discarded
  (the browser clears its input before each request).

Bench (2026-11-12, a script with the browser's timings): first contact on
the second try, then ~160-300 round trips/s with 0 bad in 812-1598; the
relay back at 11 sweeps/s about 1 s after the host went quiet. Not yet
tried from Chrome.

## Wiring

Relay pads TX (GPIO11) → node/XR4 RX, relay RX (GPIO12) ← node/XR4 TX, GND.
921600 8N1. The ROM prints its boot banner on GPIO11 at 115200 at every
reset; the node ignores it (bad checksums). Cross the wires: radio TX on
the relay's TX pad is two outputs fighting (the board got hot, and a node
wired that way stuck in ROM before reading its flash).

## On a board (2026-11-12)

First run: relay on a C5-Zero + RP4TD (ELRS AirPort TX, 2.4 GHz, F1000,
Link Mode MAVLink) to an RP4TD RX + C5 node (`1.2.0-c5.12`, API 36):
8 seats, 11 sweeps/s, round trip 5.8 ms, 0 timeouts or checksum errors in
1271 reads; connected to mach1. No flown pass yet.

`hello` reports `plugin_version` 0.5.0 (the rh_mach1 protocol it speaks;
mission control enables node calibration and channels from 0.5.0) and the
firmware version in `rh_version` (`c5-relay 0.1.0`).

**UART0 clock (the "dead board" bug).** With the console on USB, ESP-IDF's
start-up turns UART0's clocks off. The C5 ROM resets UART0 on every warm
reset (watchdog, panic, `esp_restart`, the reset opening the USB port
causes) and waits for it forever: no boot banner, USB writes time out,
esptool cannot connect. Found over JTAG (ROM polling `PCR_UART0_CONF` with
`PCR_UART0_SCLK_CONF` = 0; setting `SCLK_EN` let it boot at once).
`app_main` now switches UART0's clocks back on first. A board stuck with an
older image: hold BOOT while plugging in, or over the built-in JTAG:

```text
sudo openocd -f board/esp32c5-builtin.cfg -c "adapter serial <MAC>" \
  -c "init; halt; mww 0x60096004 0x00400000; reset halt; mww 0x60096004 0x00400000; \
      program_esp build/rhrelay_esp32c5.bin 0x10000 verify; reset run; shutdown"
```

**Open: ROM watchdog loop after a warm reset with the radio attached.**
Banner, then no `SPI mode` line, `rst:0x7 (TG0_WDT_HPSYS)` with saved PC
`0x40038598` over and over; JTAG cannot halt the hart (stuck on a bus
access, likely the flash read). Seen on the node and on the relay (after
esptool's reset), both with an RP4TD on 5 V and the UART pads; other warm
resets with the same wiring booted. A power cycle clears it. Cause not
known.

**Evidence on the next failure:** a `coredump` partition (ELF, read with
`idf.py coredump-info`), the task watchdog panics, and the first console
line is `relay boot: reset reason <n>`. `-DCMAKE_CXX_FLAGS=-DRELAY_DIAG`
builds a staged start (console, then node UART, then Wi-Fi, 8 s apart) with
a 1 s heartbeat, to see which step stops a board.

## Tests

```text
make -C test        # 222 checks: engine, driver vs a fake node, mach1 protocol,
                    # command codes vs src/node/commands.h (ASan/UBSan)
make -C test pty    # driver vs the C5 node's real core on a pty: 2 passes, lap error
```
