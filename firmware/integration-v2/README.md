# Integration-v2 STM32F411 firmware

- Node API: 38
- STM32 core: 2.12.0
- Arduino CLI: 1.5.1
- FQBN: `STMicroelectronics:stm32:GenF4:pnum=BLACKPILL_F411CE,xserial=generic,usb=none,xusb=FS,opt=osstd,rtlib=nano`
- Extra C++ flags: `-DSTM32_F4_PROCTYPE`
- SHA-256: `628501a7c8567198814df15009edba5704119f82b752865c0815e31b6c56baae`
- Image: `RH_S32_BPill_node_STM32F4_api38.bin` (22688 bytes, built Sep 29 2026 at 11:27:01)

This image combines per-node normalisation with wide-RSSI transport and
runtime ADC width selection at node API 38. Normalisation commands are gated
at API 37, so they also reach nodes running the earlier firmware.

The binary contains firmware version, processor type, and build timestamp
strings. Flash using RotorHazard's node updater, then restart the server.
See [firmware upgrade instructions](../../doc/node-firmware-upgrade.md).

Validation: Python normalisation, resolution and VTX suites, the C++ threshold
framing regression, and UI tests passed. STM32F411 and AVR Nano builds passed.
STM32 uses 22,240 bytes flash and 15,140 bytes RAM. AVR uses 12,300 bytes flash
and 1,775 bytes RAM; the compiler warns of low remaining AVR RAM (273 bytes).
Only STM32 firmware is included for deployment to this timer.
