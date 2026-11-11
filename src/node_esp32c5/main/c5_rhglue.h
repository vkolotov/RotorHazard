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

// C5-private extension of the RotorHazard serial-node protocol. Read commands
// are <= 0x50 and writes are > 0x50, matching RhLink's existing framing.
constexpr uint8_t kReadDetectionConfig = 0x41;
constexpr uint8_t kReadCalibrationConfig = 0x42;
constexpr uint8_t kWriteDetectionConfig = 0x73;
constexpr uint8_t kWriteCalibrationConfig = 0x74;
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

private:
    Transport& out_;
    ConsoleHandler console_ = nullptr;
    uint8_t cmd_ = 0;
    uint8_t buf_[72];   // the largest write payload plus its checksum
    uint8_t want_ = 0, have_ = 0;     // write command payload + checksum
    bool inConsole_ = false;
    char line_[96];
    size_t lineLen_ = 0;
    uint32_t commands_ = 0, badChecksums_ = 0;

    void byte(uint8_t b);
};

// Where a successful calibration write is saved (the firmware's flash).
void rhSetCalibrationStore(void (*store)());

// Set up the node array for this many seats and attach the scanner.
void rhBegin(Scanner& scanner, int seats);
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
