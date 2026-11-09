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
    uint8_t buf_[24];
    uint8_t want_ = 0, have_ = 0;     // write command payload + checksum
    bool inConsole_ = false;
    char line_[96];
    size_t lineLen_ = 0;
    uint32_t commands_ = 0, badChecksums_ = 0;

    void byte(uint8_t b);
};

// Set up the node array for this many seats and attach the scanner.
void rhBegin(Scanner& scanner, int seats);
// After any command: the bookkeeping rhnode.cpp's loop() does.
void rhAfterCommands();
// Seat readings from the scanner (a SeatSink).
void rhSeatSample(void* ctx, int seat, uint32_t tMs, int32_t dbQ8, bool held);
Scanner* rhScanner();

}  // namespace c5

#endif
