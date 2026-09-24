# Integration-v2 STM32F411 firmware

- Source commit: `8728afd1e63e4e78fd879cacc899843b37ee631c`
- Node API: 38
- STM32 core: 2.12.0
- Arduino CLI: 1.5.1
- FQBN: `STMicroelectronics:stm32:GenF4:pnum=BLACKPILL_F411CE,xserial=generic,usb=none,xusb=FS,opt=osstd,rtlib=nano`
- Extra C++ flags: `-DSTM32_F4_PROCTYPE`
- SHA-256: `905cdf2d7760ba2a745e618d4bcf3c22b7c837bc70ae322d76b423d5903a2a0e`
- Image: `RH_S32_BPill_node_STM32F4_api38.bin` (22740 bytes)

The binary contains firmware version, processor type, and build timestamp strings.
Flash using RotorHazard's node updater, then restart the server on integration-v2.
See [deployment and migration](../../doc/node-equalisation.md).

Validation: 38 Python tests passed. STM32F411 and AVR Nano builds passed.
STM32 uses 22,292 bytes flash and 15,172 bytes RAM. AVR uses 12,324 bytes flash
and 1,777 bytes RAM; the compiler warns of low remaining AVR RAM (271 bytes).
Only STM32 firmware is included for deployment to this timer.
