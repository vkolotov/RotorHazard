// The receiver backend for RotorHazard's RssiNode on the ESP32-C5, and the
// serial link. The command parser follows rhnode.cpp's serialEvent()
// (RotorHazard, MIT License, Copyright (c) 2019 Michael Niggel and other
// contributors).
#include "c5_rhglue.h"

#include <string.h>
#include "c5_filter.h"
#include "c5_policy.h"

#include "config.h"
#include "RssiNode.h"
#include "commands.h"

#ifndef RHNODE_C5_VERSION
#define RHNODE_C5_VERSION "1.2.0-c5.14-console-guard"
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

static RuntimeMedian* g_medianFilters = nullptr;
static LatestFilter* g_latestFilters = nullptr;
static ShortPowerMean* g_powerMeans = nullptr;
static bool g_powerMean = true;
static int g_powerMeanLength = 2;
static int g_medianLength = kMaxMedian;
static uint8_t g_detectionConfigStatus = kDetectionConfigOk;
static uint8_t g_calibrationConfigStatus = kCalibrationConfigOk;
static int g_detectionMinPassMs = 0;
static int g_detectionPoint = 0;   // index into kPolicyCurve while managed

constexpr int kDetectionPassMsMin = 1;
constexpr int kDetectionPassMsMax = 2000;
static void attachFilters()
{
    if (!g_medianFilters || !g_latestFilters || !g_powerMeans)
        return;
    for (int i = 0; i < MULTI_RHNODE_MAX; ++i) {
        if (g_powerMean) {
            g_powerMeans[i].setLength(g_powerMeanLength);
            g_latestFilters[i].reset();
            RssiNode::rssiNodeArray[i].rssiSetFilter(&g_latestFilters[i]);
        } else {
            g_medianFilters[i].setLength(g_medianLength);
            RssiNode::rssiNodeArray[i].rssiSetFilter(&g_medianFilters[i]);
        }
    }
}

static void resetDetectionState()
{
    attachFilters();
    for (int i = 0; i < RssiNode::multiRssiNodeCount; ++i)
        RssiNode::rssiNodeArray[i].rssiStateReset();
}

static void (*g_storeCalibration)() = nullptr;

void rhSetCalibrationStore(void (*store)())
{
    g_storeCalibration = store;
}

static uint8_t g_nodeId[kNodeIdSize] = {};

void rhSetNodeId(const uint8_t id[kNodeIdSize])
{
    memcpy(g_nodeId, id, kNodeIdSize);
}

static char g_nodeName[kNodeNameSize + 1] = {};
static void (*g_nameStore)(const char*) = nullptr;

void rhSetNodeName(const char* name)
{
    memset(g_nodeName, 0, sizeof(g_nodeName));
    if (name)
        memcpy(g_nodeName, name, strnlen(name, kNodeNameSize));
}

const char* rhNodeName() { return g_nodeName; }

void rhSetNameStore(void (*store)(const char*)) { g_nameStore = store; }

static void writeNodeName(Transport& out)
{
    uint8_t d[kNodeNameSize + 1] = {};
    memcpy(d, g_nodeName, kNodeNameSize);
    for (int i = 0; i < kNodeNameSize; ++i)
        d[kNodeNameSize] += d[i];
    out.write(d, sizeof(d));
}

static void applyNodeName(const uint8_t* d)
{
    char name[kNodeNameSize + 1] = {};
    memcpy(name, d, kNodeNameSize);
    rhSetNodeName(name);
    if (g_nameStore)
        g_nameStore(g_nodeName);
}

static void writeNodeId(Transport& out)
{
    uint8_t d[kNodeIdSize + 1] = {};
    memcpy(d, g_nodeId, kNodeIdSize);
    for (int i = 0; i < kNodeIdSize; ++i)
        d[kNodeIdSize] += d[i];
    out.write(d, sizeof(d));
}

static void writeCalibrationConfig(Transport& out)
{
    uint8_t d[kCalibrationConfigDataSize + 1] = {};
    d[0] = kCalibrationConfigVersion;
    d[1] = g_scanner->cal().enabled ? 1 : 0;
    d[2] = g_calibrationConfigStatus;
    const TrimTable& t = g_scanner->trim();
    for (int i = 0; i < kTrimPoints; ++i) {
        const int at = kCalibrationConfigHeaderSize + i * 2;
        d[at] = (uint8_t)((uint16_t)t.adjDbQ8[i] >> 8);
        d[at + 1] = (uint8_t)t.adjDbQ8[i];
    }
    for (int i = 0; i < kCalibrationConfigDataSize; ++i)
        d[kCalibrationConfigDataSize] += d[i];
    out.write(d, sizeof(d));
}

static void applyCalibrationConfig(const uint8_t* d)
{
    if (d[0] != kCalibrationConfigVersion) {
        g_calibrationConfigStatus = kCalibrationConfigBadVersion;
        return;
    }
    if (d[1] > 1) {
        g_calibrationConfigStatus = kCalibrationConfigBadEnabled;
        return;
    }
    if (d[2] != 0 || d[3] != 0) {
        g_calibrationConfigStatus = kCalibrationConfigBadReserved;
        return;
    }
    TrimTable candidate;
    const CalTable& table = g_scanner->cal();
    for (int i = 0; i < kTrimPoints; ++i) {
        const int at = kCalibrationConfigHeaderSize + i * 2;
        const int16_t trim = (int16_t)(((uint16_t)d[at] << 8) | d[at + 1]);
        const int32_t total = trim - table.gainAt(kTrimMhz[i]);
        if (trim < -kMaxAdjustmentDbQ8 || trim > kMaxAdjustmentDbQ8 ||
            total < -kMaxAdjustmentDbQ8 || total > kMaxAdjustmentDbQ8) {
            g_calibrationConfigStatus = kCalibrationConfigBadAdjustment;
            return;
        }
        candidate.adjDbQ8[i] = trim;
    }
    // Replace everything only after every trim has passed validation.
    g_scanner->trim() = candidate;
    g_scanner->cal().enabled = d[1] != 0;
    resetDetectionState();
    g_calibrationConfigStatus = kCalibrationConfigOk;
    if (g_storeCalibration)
        g_storeCalibration();
}

static int currentVisits()
{
    return g_scanner ? g_scanner->visitCount() : 0;
}

static void put16(uint8_t* d, int v)
{
    d[0] = (uint8_t)(v >> 8);
    d[1] = (uint8_t)v;
}

static void writeDetectionConfig(Transport& out)
{
    // Protocol v3, kDetectionConfigReadSize data bytes then the checksum:
    //  0 version   1 flags (writable, volatile, managed, extreme)
    //  2 requested pass ms   4 capture samples   6 mean readings (0: median)
    //  7 jitter x10 (counts, +-)   8 safe enter margin (counts)
    //  9 averaging window ms   11 seat revisit us (active capture length)
    // 13 last-write status
    // 14 curve, least to most jitter, six bytes per point: shortest pass ms
    //    (BE16), jitter x10, margin, samples / 16, readings | 0x80 if extreme
    uint8_t d[kDetectionConfigReadSize + 1] = {};
    const int visits = currentVisits();
    const bool managed = g_detectionMinPassMs != 0;
    const int samples = g_scanner ? g_scanner->config().captureN : 0;
    d[0] = kDetectionConfigVersion;
    d[1] = 0x03 | (managed ? 0x04 : 0) | (managed && kPolicyCurve[g_detectionPoint].extreme ? 0x08 : 0);
    put16(d + 2, g_detectionMinPassMs);
    put16(d + 4, samples);
    d[6] = g_powerMean ? (uint8_t)g_powerMeanLength : 0;
    if (managed) {
        const PolicyPoint& p = kPolicyCurve[g_detectionPoint];
        d[7] = p.jitterX10;
        d[8] = p.margin;
        put16(d + 9, policyWindowMs(p, visits));
    }
    put16(d + 11, policyRevisitUs(visits, samples > 0 ? samples : kPolicySamples));
    d[13] = g_detectionConfigStatus;
    for (int i = 0; i < kPolicyCurvePoints; ++i) {
        const PolicyPoint& p = kPolicyCurve[i];
        uint8_t* e = d + 14 + 6 * i;
        put16(e, policyBoundaryMs(p, visits));
        e[2] = p.jitterX10;
        e[3] = p.margin;
        e[4] = (uint8_t)(p.samples / 16);
        e[5] = (uint8_t)(p.readings | (p.extreme ? 0x80 : 0));
    }
    for (int i = 0; i < kDetectionConfigReadSize; ++i)
        d[kDetectionConfigReadSize] += d[i];
    out.write(d, sizeof(d));
}

// Apply the curve point for the requested pass at the current channel plan.
// Only a change touches the filters (and so crossing history).
static void applyManagedPolicy(bool force)
{
    if (!g_detectionMinPassMs || !g_scanner)
        return;
    g_detectionPoint = policyIndexFor(g_detectionMinPassMs, currentVisits());
    const PolicyPoint& p = kPolicyCurve[g_detectionPoint];
    if (!force && g_scanner->config().captureN == p.samples &&
        g_powerMean && g_powerMeanLength == p.readings)
        return;
    g_scanner->config().captureN = p.samples;
    g_powerMean = true;
    g_powerMeanLength = p.readings;
    resetDetectionState();
}

static void applyDetectionConfig(const uint8_t* d)
{
    if (d[0] != kDetectionConfigVersion) {
        g_detectionConfigStatus = kDetectionConfigBadVersion;
        return;
    }
    const int passMs = ((int)d[1] << 8) | d[2];
    if (passMs < kDetectionPassMsMin || passMs > kDetectionPassMsMax) {
        g_detectionConfigStatus = kDetectionConfigBadPassMs;
        return;
    }
    g_detectionMinPassMs = passMs;
    applyManagedPolicy(true);
    g_detectionConfigStatus = kDetectionConfigOk;
}

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
rssi_t RssiNode::rssiRead()
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
    if (!g_medianFilters) g_medianFilters = new RuntimeMedian[MULTI_RHNODE_MAX];
    if (!g_latestFilters) g_latestFilters = new LatestFilter[MULTI_RHNODE_MAX];
    if (!g_powerMeans) g_powerMeans = new ShortPowerMean[MULTI_RHNODE_MAX];
    attachFilters();
    for (int i = 0; i < seats; ++i) {
        RssiNode::rssiNodeArray[i].initRxModule();
        RssiNode::rssiNodeArray[i].rssiInit();
    }
    scanner.setSink(rhSeatSample, nullptr);
}

void rhSetPowerMeanLength(int n)
{
    if (n < 1) n = 1;
    if (n > kMaxPowerMean) n = kMaxPowerMean;
    if (g_powerMean && n == g_powerMeanLength)
        return;
    g_powerMean = true;
    g_powerMeanLength = n;
    attachFilters();
}

bool rhUsingPowerMean() { return g_powerMean; }
int rhPowerMeanLength() { return g_powerMeanLength; }

void rhSetMedianLength(int n)
{
    n = medianLengthFor(1000, (float)n);   // just normalise: odd, 1..255
    if (!g_powerMean && n == g_medianLength)
        return;
    g_powerMean = false;
    g_medianLength = n;
    attachFilters();
}

int rhMedianLength() { return g_medianLength; }

int rhDetectionMinPassMs() { return g_detectionMinPassMs; }
bool rhDetectionExtreme() { return g_detectionMinPassMs && kPolicyCurve[g_detectionPoint].extreme; }
void rhRefreshManagedDetection() { applyManagedPolicy(false); }
void rhClearManagedDetection()
{
    g_detectionMinPassMs = 0;
    g_detectionPoint = 0;
}

void rhSeatSample(void*, int seat, uint32_t tMs, int32_t dbQ8, bool held)
{
    if (seat >= RssiNode::multiRssiNodeCount || !g_scanner)
        return;
    if (g_powerMean) {
        mtime_t meanTs = 0;
        int32_t meanDbQ8 = 0;
        if (!g_powerMeans || !g_powerMeans[seat].add((mtime_t)tMs, dbQ8, held, &meanTs, &meanDbQ8))
            return;
        tMs = meanTs;
        dbQ8 = meanDbQ8;
    }
    RssiNode& node = RssiNode::rssiNodeArray[seat];
    const int raw = g_scanner->rssiCounts(dbQ8);
    node.rssiProcessValue(tMs, (rssi_t)raw);
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
        } else if (b < 0x20 || b == 0x7F) {
            // Not text: the '!' was noise (a relay being flashed sends its
            // boot output over the radio link), and this is RotorHazard's next
            // command. Without this, every request after a stray '!' was
            // swallowed as console text until a newline that never came: the
            // node went silent until it was power-cycled.
            inConsole_ = false;
            lineLen_ = 0;
            byte(b);
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
        if (cmd_ == kReadDetectionConfig) {
            writeDetectionConfig(out_);
            ++commands_;
            rhAfterCommands();
            return;
        }
        if (cmd_ == kReadCalibrationConfig) {
            writeCalibrationConfig(out_);
            ++commands_;
            rhAfterCommands();
            return;
        }
        if (cmd_ == kReadNodeName) {
            writeNodeName(out_);
            ++commands_;
            rhAfterCommands();
            return;
        }
        if (cmd_ == kReadNodeId) {
            writeNodeId(out_);
            ++commands_;
            rhAfterCommands();
            return;
        }
        if (cmd_ > 0x50) {   // writes to the node carry a payload and a checksum
            int size;
            if (cmd_ == kWriteDetectionConfig)
                size = kDetectionConfigWriteSize;
            else if (cmd_ == kWriteCalibrationConfig)
                size = kCalibrationConfigDataSize;
            else if (cmd_ == kWriteNodeName)
                size = kNodeNameSize;
            else {
                g_message.command = cmd_;
                size = (int8_t)g_message.getPayloadSize();
            }
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
        if (cmd_ == kWriteDetectionConfig) {
            applyDetectionConfig(buf_);
        } else if (cmd_ == kWriteCalibrationConfig) {
            applyCalibrationConfig(buf_);
        } else if (cmd_ == kWriteNodeName) {
            applyNodeName(buf_);
        } else {
            g_message.command = cmd_;
            g_message.buffer.size = size;
            for (uint8_t i = 0; i < size; ++i)
                g_message.buffer.data[i] = buf_[i];
            g_message.handleWriteCommand(true);
        }
        ++commands_;
        rhAfterCommands();
    } else {
        ++badChecksums_;
    }
    g_message.buffer.size = 0;
    want_ = have_ = 0;
}

}  // namespace c5
