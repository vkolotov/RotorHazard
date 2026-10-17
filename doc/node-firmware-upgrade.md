# Upgrading node firmware without bricking the board

For the S32_BPill board: one STM32F411 BlackPill driving eight receiver
nodes over RS422. The recovery path if a flash goes wrong depends on wiring
that many builds do not have, so the pre-flash checks below matter more than
usual.

## Why a bad flash can be unrecoverable

The only software route into the STM32 bootloader is the `JUMP_TO_BOOTLOADER`
command (0x7E), sent over serial to **running, enumerated firmware**.
`RHInterface.jump_to_bootloader()` iterates the detected nodes; firmware that
does not enumerate means an empty node list, so the command is never sent and
`stm32loader` times out on its first probe.

Re-powering does not help. The ROM bootloader is entered only when BOOT0 is
high at reset.

`src/server/util/stm32loader.py` defines `GPIO_RESET_PIN = 17` and
`GPIO_BOOT0_PIN = 27`, which suggests the Pi can force the bootloader. On a
board without those two lines wired to the STM32 it cannot: the pins are
driven and nothing is listening. Check your build before relying on them.

**Without those lines, recovery is physical:** hold BOOT0 high, tap NRST,
release BOOT0, then flash immediately.

## Pre-flash checks

Run all four. Each one has caught a real failure.

### 1. The binary contains firmware strings

```bash
strings RH_S32_BPill_node_STM32F4.bin | grep FIRMWARE_
```

Expect four lines - `FIRMWARE_VERSION`, `FIRMWARE_PROCTYPE`,
`FIRMWARE_BUILDDATE`, `FIRMWARE_BUILDTIME`. **Empty output means do not
flash.**

This is the tell for a build made with the wrong toolchain. A binary built
with STM32 core 3.0.0 flashes, verifies, and then never enumerates - no
revision code, `Unable to fetch revision code for serial node` - and it
carries no firmware strings. An unmodified source build on 3.0.0 fails the
same way, so it is the toolchain, not the code.

### 2. Size is in the right range

A good build lands within a few hundred bytes of stock. Stock is 22128
bytes; builds carrying the equalisation and 12-bit work measure 22740. A
3.0.0 build is around 19.6 KB - about 2.5 KB short.

### 3. Build timestamp matches the source you intend to ship

```bash
strings <image>.bin | grep FIRMWARE_BUILDTIME
```

Compare against the commit you meant to build. A stale image in the output
directory looks identical in every other respect. This has shipped twice:
once flashing a pre-fix image, once flashing an image built before the API
level changed.

### 4. Node API level agrees on both sides

```bash
grep 'define NODE_API_LEVEL' src/node/commands.h
grep '^NODE_API_BEST' src/server/server.py
```

They must match. If the firmware is higher, the server logs *"Node firmware
is newer than this server version supports"* on every boot; lower, and it
offers a flash update that is already applied.

## Building

STM32 core **2.12.0**. Not 3.0.0 - see check 1.

```
FQBN: STMicroelectronics:stm32:GenF4:pnum=BLACKPILL_F411CE,
      xserial=generic,usb=none,xusb=FS,opt=osstd,rtlib=nano
Extra flags: -DSTM32_F4_PROCTYPE
```

Pass `--clean` and wipe `/root/.cache/arduino` between builds. The cache
survives and silently serves stale objects, which is one way a stale
timestamp gets shipped.

Build the AVR target too, even when only shipping STM32. The ATmega328 has
2048 bytes of SRAM and the node code sits at about 1777 of them; a change
that widens a buffer can break AVR while STM32 stays comfortable.

Keep a known-good image on the timer. `firmware/` holds stock
`RH_S32_BPill_node_STM32F4.bin` for exactly this.

## Flashing

Flashing needs **no sudo and no service stop**. The running server performs
the bootloader jump itself, so stopping it first removes the only thing that
can get you into the bootloader.

### From the web UI

Settings, or `/updatenodes` directly. The standalone page works even when
zero nodes are detected - on detection failure the server calls
`set_mock_fwupd_serial_obj()` and shows a flash-update banner, which is the
route back from a partially bad flash that still enumerates.

### Over socket.io

```python
import base64, socketio
AUTH = base64.b64encode(b'admin:password').decode()
sio = socketio.Client()
sio.connect('http://127.0.0.1:5000', headers={'Authorization': 'Basic ' + AUTH})
sio.emit('check_bpillfw_file', {'src_file_str': '/abs/path/to/image.bin'})
# wait for upd_set_info_text / upd_enable_update_button
sio.emit('do_bpillfw_update', {'src_file_str': '/abs/path/to/image.bin'})
# watch upd_messages_append until "Node update succeeded"
```

`check_bpillfw_file` first: it reports the version, processor type and build
timestamp found in the image alongside the running firmware, which is check 3
done for you.

`do_bpillfw_update` stops the background threads, sends
`JUMP_TO_BOOTLOADER`, closes the serial port itself, runs stm32loader, then
requires a server restart.

### From the command line

```bash
python server.py --jumptobl --flashbpill /abs/path/to/image.bin
```

Needs the serial port free, so the server must be stopped - which also means
the running firmware cannot perform the jump. Prefer the socket.io or UI
route on a board without BOOT0 wiring.

## Confirming the flash

`Verification OK` from stm32loader, then restart the server and check the
enumeration line:

```
Serial multi-node found at port '/dev/ttyAMA0', count=8, API_level=38,
  fw_version=1.2.0, fw_type=STM32F4, fw_timestamp: <date> <time>
```

Check `count`, `API_level` and `fw_timestamp` against what you meant to
flash. A successful verify only proves the bytes landed; the timestamp proves
they were the right bytes.

## After a resolution change

Stored EnterAt/ExitAt are measured against whatever the node reports, so
changing the ADC width changes what they mean. The server converts them
automatically. If you flash firmware that changes the width without the
matching server, convert or re-set the thresholds by hand - every node
otherwise sits permanently crossing or permanently deaf.
