// Minimal Arduino surface for building RotorHazard's node core
// (RssiNode.cpp, commands.cpp) on the ESP32-C5 under ESP-IDF, and on a PC
// for the host tests. Only what the shared node code uses is provided; the
// receiver itself is the C5's radio (see c5_rhglue.cpp), so there are no
// pins to drive.
#ifndef RHNODE_C5_ARDUINO_H
#define RHNODE_C5_ARDUINO_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t byte;

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define HEX 16

#ifdef __cplusplus
extern "C" {
#endif
// Provided by the platform: esp_timer on the C5, a settable clock in tests.
uint32_t rhc5_millis(void);
uint32_t rhc5_micros(void);
void rhc5_delay_ms(uint32_t ms);
#ifdef __cplusplus
}
#endif

static inline uint32_t millis(void) { return rhc5_millis(); }
static inline uint32_t micros(void) { return rhc5_micros(); }
static inline void delay(uint32_t ms) { rhc5_delay_ms(ms); }
static inline void delayMicroseconds(uint32_t) {}
static inline void pinMode(int, int) {}
static inline void digitalWrite(int, int) {}
static inline int digitalRead(int) { return 0; }

#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? ((value) |= (1UL << (bit))) : ((value) &= ~(1UL << (bit))))

#endif
