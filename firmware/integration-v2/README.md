# Integration-v2 STM32F411 firmware

- Node API: 37
- STM32 core: 2.12.0
- Arduino CLI: 1.5.1
- FQBN: `STMicroelectronics:stm32:GenF4:pnum=BLACKPILL_F411CE,xserial=generic,usb=none,xusb=FS,opt=osstd,rtlib=nano`
- Extra C++ flags: `-DSTM32_F4_PROCTYPE`
- SHA-256: `d8946246b34999819144bd194bcc64bccfb817aeb5093ac5fae2c6f439e49105`
- Image: `RH_S32_BPill_node_STM32F4_api37.bin` (22740 bytes)

The binary contains firmware version, processor type, and build timestamp strings.
Flash using RotorHazard's node updater, then restart the server on integration-v2.
See [deployment and migration](../../doc/node-equalisation.md).

Validation: the Python integration suite and the C++ threshold framing regression
passed. STM32F411 and AVR Nano builds passed. STM32 uses 22,292 bytes flash and
15,172 bytes RAM. AVR uses 12,324 bytes flash and 1,777 bytes RAM; the compiler
warns of low remaining AVR RAM (271 bytes). Only STM32 firmware is included for
deployment to this timer.

This image includes the threshold payload-length fix. Development builds numbered
API 37 (earlier equalisation protocol) and API 38 were flashed to the project timer
while this work was in progress; neither was released, and this image replaces them.
