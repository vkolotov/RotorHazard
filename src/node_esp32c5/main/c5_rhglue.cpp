// The receiver backend for RotorHazard's RssiNode on the ESP32-C5, and the
// serial link. The command parser follows rhnode.cpp's serialEvent()
// (RotorHazard, MIT License, Copyright (c) 2019 Michael Niggel and other
// contributors).
#include "c5_rhglue.h"

#include "config.h"
#include "RssiNode.h"
#include "commands.h"

#ifndef RHNODE_C5_VERSION
#define RHNODE_C5_VERSION "1.2.0-c5.0"
#endif

// informational strings read by RH (prefixes let them be found in the .bin)
const char *firmwareVersionString = "FIRMWARE_VERSION: " RHNODE_C5_VERSION;
const char *firmwareBuildDateString = "FIRMWARE_BUILDDATE: " __DATE__;
const char *firmwareBuildTimeString = "FIRMWARE_BUILDTIME: " __TIME__;
const char *firmwareProcTypeString = "FIRMWARE_PROCTYPE: ESP32C5";

// SEND_STATUS_MESSAGE: shutdown button and server-idle ticks are for the
// S32_BPill's buzzer and LEDs; the C5 has neither.
void handleStatusMessage(byte, byte) {}

// Not offered (RHFEAT_JUMPTO_BOOTLDR is clear): the C5 has no STM32 bootloader.
void doJumpToBootloader() {}

namespace c5 {

static Scanner* g_scanner = nullptr;

Scanner* rhScanner() { return g_scanner; }

}  // namespace c5

// --- RssiNode receiver backend (RHNODE_RADIO_BACKEND) ---

void RssiNode::initRx5808Pins(int nIdx)
{
    nodeIndex = nIdx;
    slotIndex = nIdx;
}

void RssiNode::initRxModule()
{
    setRxModuleToFreq(settings.vtxFreq);
}

void RssiNode::setRxModuleToFreq(uint16_t vtxFreq)
{
    rxPoweredDown = (vtxFreq == 1111);
    recentSetFreqFlag = false;   // the C5 retunes per capture; nothing to settle
    lastSetFreqTimeMs = millis();
    if (c5::g_scanner)
        c5::g_scanner->setSeatFreq(nodeIndex, rxPoweredDown ? 0 : vtxFreq);
}

// RH checks this after every frequency write: true when the seat is being
// measured on the frequency it was given.
bool RssiNode::testRxModuleRegister()
{
    if (!c5::g_scanner)
        return false;
    return c5::Scanner::supported(settings.vtxFreq) &&
           c5::g_scanner->seatFreq(nodeIndex) == settings.vtxFreq;
}

// Readings arrive from the scanner (rhSeatSample), not by polling.
int RssiNode::rssiReadRaw()
{
    return 0;
}

namespace c5 {

void rhBegin(Scanner& scanner, int seats)
{
    g_scanner = &scanner;
    if (seats < 1)
        seats = 1;
    if (seats > MULTI_RHNODE_MAX)
        seats = MULTI_RHNODE_MAX;
    for (int i = 0; i < MULTI_RHNODE_MAX; ++i)
        RssiNode::rssiNodeArray[i].initRx5808Pins(i);
    RssiNode::multiRssiNodeCount = (uint8_t)seats;
    for (int i = 0; i < seats; ++i) {
        RssiNode::rssiNodeArray[i].initRxModule();
        RssiNode::rssiNodeArray[i].rssiInit();
    }
    scanner.setSink(rhSeatSample, nullptr);
}

void rhSeatSample(void*, int seat, uint32_t tMs, int32_t dbQ8, bool)
{
    if (seat >= RssiNode::multiRssiNodeCount || !g_scanner)
        return;
    RssiNode& node = RssiNode::rssiNodeArray[seat];
    const bool full = node.getAdcResolution() == FULL_ADC_BITS;
    const int raw = g_scanner->rssiCounts(dbQ8, full);
    node.rssiProcessValue(tMs, node.rssiNormalise(raw));
}

void rhAfterCommands()
{
    // as rhnode.cpp's loop(): a READ_LAP_STATS also activates the current
    // node, so operations resume after a node or link reset
    const uint8_t flags = settingChangedFlags;
    settingChangedFlags = 0;
    RssiNode* node = getCmdRssiNodePtr();
    if (!node->getActivatedFlag() && (flags & LAPSTATS_READ))
        node->setActivatedFlag(true);
}

// --- serial link ---

static Message g_message;   // RH's own command handlers work on a Message

void RhLink::feed(const uint8_t* data, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        byte(data[i]);
}

void RhLink::byte(uint8_t b)
{
    if (inConsole_) {
        if (b == '\r' || b == '\n') {
            line_[lineLen_] = '\0';
            inConsole_ = false;
            if (console_)
                console_(line_, out_);
        } else if (lineLen_ + 1 < sizeof(line_)) {
            line_[lineLen_++] = (char)b;
        }
        return;
    }

    if (want_ == 0) {
        // a new command
        if (b == '!') {
            inConsole_ = true;
            lineLen_ = 0;
            return;
        }
        cmd_ = b;
        if (cmd_ > 0x50) {   // writes to the node carry a payload and a checksum
            g_message.command = cmd_;
            const int size = (int8_t)g_message.getPayloadSize();
            if (size > 0 && size + 1 <= (int)sizeof(buf_)) {
                want_ = (uint8_t)(size + 1);
                have_ = 0;
            }
        } else {
            g_message.command = cmd_;
            g_message.handleReadCommand(true);
            ++commands_;
            if (g_message.buffer.size > 0) {
                out_.write(g_message.buffer.data, g_message.buffer.size);
                g_message.buffer.size = 0;
            }
            rhAfterCommands();
        }
        return;
    }

    buf_[have_++] = b;
    if (have_ < want_)
        return;
    const uint8_t size = (uint8_t)(want_ - 1);
    uint8_t sum = 0;
    for (uint8_t i = 0; i < size; ++i)
        sum += buf_[i];
    if (sum == buf_[size]) {
        g_message.command = cmd_;
        g_message.buffer.size = size;
        for (uint8_t i = 0; i < size; ++i)
            g_message.buffer.data[i] = buf_[i];
        g_message.handleWriteCommand(true);
        ++commands_;
        rhAfterCommands();
    } else {
        ++badChecksums_;
    }
    g_message.buffer.size = 0;
    want_ = have_ = 0;
}

}  // namespace c5
