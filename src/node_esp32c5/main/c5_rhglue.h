// RotorHazard's node core (RssiNode, commands) on the ESP32-C5: the
// receiver backend RssiNode needs, the serial command link, and the path
// from a seat's band-power reading into RssiNode::rssiProcessValue().
//
// Portable: the host tests link this with RH's own RssiNode.cpp and
// commands.cpp.
#ifndef RHNODE_C5_RHGLUE_H
#define RHNODE_C5_RHGLUE_H

#include <stddef.h>
#include <stdint.h>

#include "c5_scan.h"

namespace c5 {

class Transport;

// C5-private extension of the RotorHazard serial-node protocol. Read commands
// are <= 0x50 and writes are > 0x50, matching RhLink's existing framing.
constexpr uint8_t kReadDetectionConfig = 0x41;
constexpr uint8_t kReadCalibrationConfig = 0x42;
constexpr uint8_t kWriteDetectionConfig = 0x73;
constexpr uint8_t kWriteCalibrationConfig = 0x74;
// The node's identity (read 0x43, 6 bytes): the chip's factory MAC, so a
// timer is known as the same gate however it is reached (a laptop over USB or
// the AirPort link, or a C5 relay over Wi-Fi). Read-only, never changes.
constexpr uint8_t kReadNodeId = 0x43;
constexpr uint8_t kNodeIdSize = 6;
// The node's name (read 0x44 / write 0x75, 32 bytes of UTF-8, zero padded):
// what mach1 and the relay call this timer. Saved on the node, so a rename
// through mach1 or the relay follows the node wherever it is plugged in.
constexpr uint8_t kReadNodeName = 0x44;
constexpr uint8_t kWriteNodeName = 0x75;
constexpr uint8_t kNodeNameSize = 32;
// The node's health (read 0x45, kNodeStatsSize bytes, behind feature flag
// 0x0100), for mission control's timer statistics. Big-endian:
//   0 version (1), 1 reset reason (esp_reset_reason_t), 2 flags (1 USB host
//   connected, 2 UART link on), 3 reserved,
//   4 uptime s, 8 free heap, 12 lowest free heap,
//   16 scan cycles, 20 captures, 24 failed, 28 held, 32 clipped captures,
//   36 longest cycle us,
//   40 UART link commands, 44 its bad checksums, 48 its noise (a stray '!'
//   recovered), 52 USB link commands (all u32).
// Counters run from boot (the console's `stats` clears the scan ones).
constexpr uint8_t kReadNodeStats = 0x45;
constexpr uint8_t kNodeStatsVersion = 1;
constexpr uint8_t kNodeStatsSize = 56;
constexpr uint8_t kNodeStatsUsbHost = 1;
constexpr uint8_t kNodeStatsUartLink = 2;

// Battery voltage (read 0x48, kBatterySize bytes, behind feature flag 0x0400):
// a resistor divider from the battery to an ADC pin. Big-endian:
//   0 version (1), 1 flags (1 = a divider is wired: the pin reads at least
//   kBatteryPinMinMv), 2 pin mV, 4 battery mV (pin x ratio), 6 divider ratio
//   x1000 (u16 each). The pin and battery read 0 until the first sample.
// WRITE 0x7F (2 bytes, ratio x1000 BE16) sets and saves the ratio; 0 restores
// the default. A known battery voltage calibrates it: ratio = battery / pin.
constexpr uint8_t kReadBattery = 0x48;
constexpr uint8_t kWriteBatteryRatio = 0x7F;
constexpr uint8_t kBatteryVersion = 1;
constexpr uint8_t kBatterySize = 8;
constexpr uint8_t kBatteryWired = 1;
constexpr uint16_t kBatteryPinMinMv = 150;
constexpr uint16_t kBatteryRatioMin = 1000, kBatteryRatioMax = 20000;
struct NodeBattery {
    uint16_t pinMv = 0, batteryMv = 0, ratioMilli = 0;
    uint8_t flags = 0;
};

// Reliable application update. Both direct RotorHazard serial and a relay
// over AirPort speak exactly these messages. Writes retain RH's outer
// additive checksum; STATUS acknowledges the next byte offset, making a
// repeated chunk harmless after a lost reply.
constexpr uint8_t kReadFwUpdateInfo = 0x46;
constexpr uint8_t kReadFwUpdateStatus = 0x47;
constexpr uint8_t kWriteFwUpdateBegin = 0x76;
constexpr uint8_t kWriteFwUpdateData = 0x77;
constexpr uint8_t kWriteFwUpdateFinish = 0x7B;
constexpr uint8_t kWriteFwUpdateAbort = 0x7C;
constexpr uint8_t kWriteFwUpdateConfirm = 0x7D;
constexpr uint8_t kFwUpdateProtocol = 1;
constexpr uint8_t kFwUpdateChunkSize = 64;
constexpr uint8_t kFwUpdateInfoSize = 16;
constexpr uint8_t kFwUpdateStatusSize = 16;
constexpr uint8_t kFwUpdateBeginSize = 41; // protocol, session, size, SHA-256
constexpr uint8_t kFwUpdateDataSize = 9 + kFwUpdateChunkSize; // session, offset, length, padded data

// Device implementation in c5_ota.cpp. Weak no-op definitions keep the
// portable host tests independent of ESP-IDF.
void fwUpdateWriteInfo(Transport& out);
void fwUpdateWriteStatus(Transport& out);
int fwUpdatePayloadSize(uint8_t command);
void fwUpdateHandle(uint8_t command, const uint8_t* data, size_t size);
bool fwUpdateBusy();
void fwUpdateInit();
void fwUpdateTick();

// What the firmware knows of itself that the glue cannot: filled on each read.
struct NodeSystem {
    uint32_t uptimeS = 0, heap = 0, heapMin = 0;
    uint8_t resetReason = 0, flags = 0;
};

constexpr uint8_t kDetectionConfigVersion = 3;
constexpr uint8_t kDetectionConfigWriteSize = 3;  // version, minimum pass ms BE16
constexpr uint8_t kDetectionConfigReadSize = 86;   // 14 + 12 curve points x 6; see writeDetectionConfig
// Calibration v2 (read 0x42 / write 0x74, 36 bytes): version 2, enabled,
// status on read (reserved 0 on write), reserved 0, then the sixteen trims
// L1-L8, R1-R8 (kTrimMhz) as signed dB Q8 BE16; positive raises the RSSI.
// A write replaces enabled and all trims and saves them; the calibration
// table underneath is set on the console only.
constexpr uint8_t kCalibrationConfigVersion = 2;
constexpr uint8_t kCalibrationConfigHeaderSize = 4;
constexpr uint8_t kCalibrationConfigDataSize = kCalibrationConfigHeaderSize + kTrimPoints * 2;

enum DetectionConfigStatus : uint8_t {
    kDetectionConfigOk = 0,
    kDetectionConfigBadVersion = 1,
    kDetectionConfigBadPassMs = 2,
};

enum CalibrationConfigStatus : uint8_t {
    kCalibrationConfigOk = 0,
    kCalibrationConfigBadVersion = 1,
    kCalibrationConfigBadEnabled = 2,
    kCalibrationConfigBadReserved = 3,
    kCalibrationConfigBadAdjustment = 4,   // a trim, or table plus trim, beyond +-20 dB
};

// Where a link's replies go.
class Transport {
public:
    virtual ~Transport() {}
    virtual void write(const uint8_t* data, size_t n) = 0;
};

// A console line (after the '!') and where to print the answer.
typedef void (*ConsoleHandler)(const char* line, Transport& out);

// The RotorHazard serial node protocol on one byte stream. RH never sends
// 0x21 ('!'), so a line starting with it, received between commands, is a
// console command instead.
class RhLink {
public:
    explicit RhLink(Transport& out) : out_(out) {}
    void feed(const uint8_t* data, size_t n);
    void setConsole(ConsoleHandler h) { console_ = h; }
    uint32_t commands() const { return commands_; }
    uint32_t badChecksums() const { return badChecksums_; }
    // A '!' that turned out to be noise (not a console line): recovered from.
    uint32_t noise() const { return noise_; }

private:
    Transport& out_;
    ConsoleHandler console_ = nullptr;
    uint8_t cmd_ = 0;
    uint8_t buf_[80];   // the largest write payload plus its checksum
    uint8_t want_ = 0, have_ = 0;     // write command payload + checksum
    bool inConsole_ = false;
    char line_[96];
    size_t lineLen_ = 0;
    uint32_t commands_ = 0, badChecksums_ = 0, noise_ = 0;

    void byte(uint8_t b);
};

// READ 0x45: the firmware's part (uptime, heap, reset reason, flags), and
// the links whose counters it reports (either may be null).
void rhSetSystemInfo(void (*fill)(NodeSystem* out));
void rhSetStatsLinks(const RhLink* usb, const RhLink* uart);
// READ 0x48 / WRITE 0x7F: the firmware's battery reading and ratio store
// (without them the node answers "not wired").
void rhSetBattery(void (*read)(NodeBattery* out), bool (*setRatio)(uint16_t ratioMilli));

// Where a successful calibration write is saved (the firmware's flash).
void rhSetCalibrationStore(void (*store)());

// Set up the node array for this many seats and attach the scanner.
void rhBegin(Scanner& scanner, int seats);
// The identity READ 0x43 answers (the factory MAC on the C5; tests set their own).
void rhSetNodeId(const uint8_t id[kNodeIdSize]);
// The name READ 0x44 answers; a WRITE 0x75 replaces it and calls the store (NVS on the C5).
void rhSetNodeName(const char* name);
const char* rhNodeName();
void rhSetNameStore(void (*store)(const char* name));
// After any command: the bookkeeping rhnode.cpp's loop() does.
void rhAfterCommands();
// Seat readings from the scanner (a SeatSink).
void rhSeatSample(void* ctx, int seat, uint32_t tMs, int32_t dbQ8, bool held);
Scanner* rhScanner();
// Smoothing. The short mean is computed in linear power before RSSI
// RSSI mapping. Changing mode/length restarts every seat's filter.
void rhSetPowerMeanLength(int n);
bool rhUsingPowerMean();
int rhPowerMeanLength();
void rhSetMedianLength(int n);
int rhMedianLength();
// Managed detection: one input, the shortest pass to catch (ms). The
// firmware picks the longest mean that still detects it (c5_policy.h) and
// re-checks it when the channel plan changes.
int rhDetectionMinPassMs();
bool rhDetectionExtreme();
void rhRefreshManagedDetection();
void rhClearManagedDetection();

}  // namespace c5

#endif
