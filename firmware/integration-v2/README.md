# Integration-v2 STM32F411 firmware

- Source commit: `dd0b9c63e833d715bcc8157a1088449d8c303431`
- Node API: 38
- STM32 core: 2.12.0
- Arduino CLI: 1.5.1
- FQBN: `STMicroelectronics:stm32:GenF4:pnum=BLACKPILL_F411CE,xserial=generic,usb=none,xusb=FS,opt=osstd,rtlib=nano`
- Extra C++ flags: `-DSTM32_F4_PROCTYPE`
- SHA-256: `8bfa1a06034e74b8359d2c139d7164dc2487bc9f2f54533b3df545192874fd66`
- Image: `RH_S32_BPill_node_STM32F4_api38.bin` (22740 bytes)

The binary contains firmware version, processor type, and build timestamp strings.
Flash using RotorHazard's node updater, then restart the server on integration-v2.
See [deployment and migration](../../doc/node-equalisation.md).

Validation: the 38-test Python suite and the added C++ threshold framing regression passed. STM32F411 and AVR Nano builds passed.
STM32 uses 22,292 bytes flash and 15,172 bytes RAM. AVR uses 12,324 bytes flash
and 1,777 bytes RAM; the compiler warns of low remaining AVR RAM (271 bytes).
Only STM32 firmware is included for deployment to this timer.

This image includes the threshold payload-length fix. The earlier image at commit
`c1cf2f94` incorrectly framed 16-bit threshold writes and must not be reused.
