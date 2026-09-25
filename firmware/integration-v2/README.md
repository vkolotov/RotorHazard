# Integration-v2 STM32F411 firmware

- Node API: 38
- STM32 core: 2.12.0
- Arduino CLI: 1.5.1
- FQBN: `STMicroelectronics:stm32:GenF4:pnum=BLACKPILL_F411CE,xserial=generic,usb=none,xusb=FS,opt=osstd,rtlib=nano`
- Extra C++ flags: `-DSTM32_F4_PROCTYPE`
- SHA-256: `bd2e4dbeeeb11dbd4c33ea21f96c91518cd79f64c6ff3ac885c16de61b81d4d4`
- Image: `RH_S32_BPill_node_STM32F4_api38.bin` (22740 bytes)

This branch is both pull requests as they stand: per-node equalisation at
node API 37, and the wide-RSSI transport plus runtime ADC width at 38 on top
of it. Equalisation commands are gated at 37, so they still reach a node
running only that firmware.

The binary contains firmware version, processor type, and build timestamp
strings. Flash using RotorHazard's node updater, then restart the server.
See [deployment and migration](../../doc/node-equalisation.md).

Validation: both Python suites and the C++ threshold framing regression
passed. STM32F411 and AVR Nano builds passed. STM32 uses 22,292 bytes flash
and 15,172 bytes RAM. AVR uses 12,324 bytes flash and 1,777 bytes RAM; the
compiler warns of low remaining AVR RAM (271 bytes). Only STM32 firmware is
included for deployment to this timer.
