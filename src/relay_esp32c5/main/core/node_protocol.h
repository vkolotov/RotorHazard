// RotorHazard's serial node protocol, as RotorHazard's server speaks it
// (src/interface/serial_node.py, RHInterface.py, src/node/commands.h).
//
// Read:  host sends [cmd]; the node answers `size` data bytes + 1 checksum.
// Write: host sends [cmd, ...data, checksum]; the node does not answer.
// Checksum = sum(data) & 0xFF over the data only. Multi-byte values are
// big-endian.
//
// The command codes are repeated here rather than taken from
// ../node/commands.h, which pulls in the node's I/O layer. test/test_main.cpp
// checks every one against commands.h, so they cannot drift.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace relay {

constexpr uint8_t READ_FREQUENCY = 0x03;
constexpr uint8_t TEST_RX_REGISTER = 0x04;
constexpr uint8_t READ_LAP_PASS_STATS = 0x0D;
constexpr uint8_t READ_LAP_EXTREMUMS = 0x0E;
constexpr uint8_t READ_REVISION_CODE = 0x22;
constexpr uint8_t READ_ENTER_AT_LEVEL = 0x31;
constexpr uint8_t READ_EXIT_AT_LEVEL = 0x32;
constexpr uint8_t READ_TIME_MILLIS = 0x33;
constexpr uint8_t READ_MULTINODE_COUNT = 0x39;
constexpr uint8_t READ_CURNODE_INDEX = 0x3A;
constexpr uint8_t READ_FW_VERSION = 0x3D;
constexpr uint8_t READ_FW_PROCTYPE = 0x40;
constexpr uint8_t WRITE_FREQUENCY = 0x51;
constexpr uint8_t WRITE_ENTER_AT_LEVEL = 0x71;
constexpr uint8_t WRITE_EXIT_AT_LEVEL = 0x72;
constexpr uint8_t WRITE_CURNODE_INDEX = 0x7A;

constexpr uint8_t LAPSTATS_FLAG_CROSSING = 0x01;
constexpr size_t FW_TEXT_BLOCK_SIZE = 16;
// The high byte of READ_REVISION_CODE: proves a RotorHazard node answered.
constexpr uint8_t REVISION_MAGIC = 0x25;
constexpr int MIN_API_LEVEL = 35;
// RotorHazard writes this to power a seat's receiver down (API >= 24).
constexpr int DISABLED_FREQUENCY = 1111;

inline uint8_t checksum(const uint8_t* data, size_t n)
{
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += data[i];
    return (uint8_t)(sum & 0xFF);
}

inline uint16_t unpack16(const uint8_t* d) { return (uint16_t)((d[0] << 8) | d[1]); }

// Whether the node sends RSSI as 16 bits: STM32 and ESP32 builds at API 38
// widen it (RotorHazard's Node.has_wide_rssi()). The C5 main line is API 36,
// 8-bit.
bool hasWideRssi(int apiLevel, const std::string& procType);
inline size_t lapStatsSize(bool wide) { return wide ? 11 : 8; }
inline int maxRssi(bool wide) { return wide ? 0xFFFF : 255; }

struct LapPassStats {
    uint8_t lapId;     // increments with each completed crossing (wraps)
    uint16_t msSince;  // ms from that crossing to when the node answered
    int rssi;
    int nodePeakRssi;
    int passPeakRssi;
    uint16_t loopTimeUs;
};

// READ_LAP_PASS_STATS: lap(1) ms(2) rssi nodePeak passPeak (1 or 2 each) loopTime(2).
LapPassStats parseLapPassStats(const uint8_t* data, bool wide);

// READ_LAP_EXTREMUMS: flags(1) passNadir nodeNadir, then one history extremum.
inline bool extremumsCrossing(const uint8_t* data) { return (data[0] & LAPSTATS_FLAG_CROSSING) != 0; }

// A firmware text block ("1.2.0-c5.4", "ESP32C5"), NUL padding removed.
std::string fwText(const uint8_t* data, size_t n);

}  // namespace relay
