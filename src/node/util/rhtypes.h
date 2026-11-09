#ifndef rhtypes_h
#define rhtypes_h

#include <inttypes.h>

// 32-bit node processors (STM32, ESP32-C5) carry RSSI as 16 bits; AVR stays
//  at 8. One switch, so every file that depends on the width agrees.
#if defined(STM32_CORE_VERSION) || defined(RHNODE_ESP32C5)
#define RHNODE_WIDE_RSSI 1
#else
#define RHNODE_WIDE_RSSI 0
#endif

// semantic types
typedef uint32_t mtime_t; // milliseconds
typedef uint32_t utime_t; // micros
// On STM32 the ADC is read at its full 12-bit width, so an RSSI value needs
//  more than a byte. AVR nodes stay 8-bit: a 16-bit rssi_t doubles the
//  255-entry median buffers and overflows an ATmega328's 2 KB of SRAM.
#if RHNODE_WIDE_RSSI
typedef uint16_t rssi_t;
#else
typedef uint8_t rssi_t;
#endif

struct Extremum
{
  rssi_t volatile rssi;
  mtime_t volatile firstTime;
  uint16_t volatile duration;
};

#if RHNODE_WIDE_RSSI
#define MAX_RSSI 0xFFFF
#else
#define MAX_RSSI 0xFF
#endif
#define isPeakValid(x) ((x).rssi != 0)
#define isNadirValid(x) ((x).rssi != MAX_RSSI)
#define invalidatePeak(x) ((x).rssi = 0)
#define invalidateNadir(x) ((x).rssi = MAX_RSSI)

#endif
