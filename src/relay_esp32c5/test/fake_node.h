// A RotorHazard node on a virtual clock, for the relay's host tests: it
// answers the protocol the way RssiNode/commands.cpp do, and can lose or
// garble replies. A port of mach1's browserTimer/fakeNode.ts.
#pragma once
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "node_driver.h"
#include "node_protocol.h"

struct FakeSeat {
    int frequency = 5800;
    int enter = 120;
    int exit = 90;
    int rssi = 40;
    int lapId = 0;
    int64_t lastCrossUs = 0;
    bool crossing = false;
};

class FakeNode : public relay::NodeLink {
public:
    explicit FakeNode(int seats = 8, int api = 36, const char* proc = "ESP32C5") : api(api), proc(proc)
    {
        this->seats.resize(seats);
    }

    int api;
    std::string proc;
    std::string firmware = "1.2.0-c5.4";
    std::vector<FakeSeat> seats;
    int64_t clockUs = 1000000;
    int64_t replyDelayUs = 6000;  // request sent -> whole reply received
    int dropNext = 0;             // replies to lose
    int garbageNext = 0;          // replies replaced by all zeros
    std::map<uint8_t, int> dropCommands;
    std::vector<std::vector<uint8_t>> writes;  // every write frame received
    // C5 settings (c5Config.ts): detection (runtime only) and calibration (36 bytes).
    bool c5 = true;
    int minPassMs = 0;  // 0: detection not set (the receiver forgets it at power-off)
    std::vector<uint8_t> calibration = std::vector<uint8_t>(36, 0);
    // Identity (1.2.0-c5.13): id and the name it keeps; an older C5 has neither.
    bool hasIdentity = true;
    std::vector<uint8_t> nodeId = {0x3c, 0x84, 0x27, 0x0a, 0x1b, 0x2c};
    std::string nodeName = "Start gate";
    // Health (1.2.0-c5.15, READ 0x45): its uptime, which a restart starts again.
    bool hasStats = false;
    uint32_t uptimeS = 3600;

    bool wide() const { return relay::hasWideRssi(api, proc); }

    // A pass completed on `seat` at virtual time `atUs`.
    void cross(int seat, int64_t atUs)
    {
        seats[seat].lapId = (seats[seat].lapId + 1) & 0xFF;
        seats[seat].lastCrossUs = atUs;
    }

    void flushInput() override { out_.clear(); }

    void write(const uint8_t* data, size_t n) override
    {
        in_.insert(in_.end(), data, data + n);
        parse();
    }

    size_t read(uint8_t* data, size_t n, uint32_t timeoutMs) override
    {
        if (out_.size() < n) {
            // Whatever came, after waiting out the timeout.
            clockUs += (int64_t)timeoutMs * 1000;
            const size_t got = out_.size();
            for (size_t i = 0; i < got; i++)
                data[i] = out_[i];
            out_.clear();
            return got;
        }
        clockUs += replyDelayUs;
        for (size_t i = 0; i < n; i++) {
            data[i] = out_.front();
            out_.pop_front();
        }
        return n;
    }

    int64_t nowUs() override { return clockUs; }
    void sleepMs(uint32_t ms) override { clockUs += (int64_t)ms * 1000; }

private:
    std::deque<uint8_t> in_;
    std::deque<uint8_t> out_;
    int current_ = 0;

    size_t writeSize(uint8_t command) const
    {
        switch (command) {
        case relay::WRITE_FREQUENCY: return 2;
        case relay::WRITE_ENTER_AT_LEVEL:
        case relay::WRITE_EXIT_AT_LEVEL: return wide() ? 2 : 1;
        case relay::WRITE_CURNODE_INDEX: return 1;
        case 0x73: return c5 ? 3 : 0;   // C5 detection
        case 0x74: return c5 ? 36 : 0;  // C5 calibration
        case 0x75: return c5 && hasIdentity ? 32 : 0;  // node name
        default: return 0;
        }
    }

    void reply(uint8_t command, std::vector<uint8_t> data)
    {
        if (dropNext > 0) {
            dropNext--;
            return;
        }
        auto it = dropCommands.find(command);
        if (it != dropCommands.end() && it->second > 0) {
            it->second--;
            return;
        }
        if (garbageNext > 0) {
            garbageNext--;
            std::fill(data.begin(), data.end(), 0);
        }
        for (uint8_t b : data)
            out_.push_back(b);
        out_.push_back(relay::checksum(data.data(), data.size()));
    }

    void put16(std::vector<uint8_t>& d, int v)
    {
        d.push_back((uint8_t)(v >> 8));
        d.push_back((uint8_t)v);
    }
    void putRssi(std::vector<uint8_t>& d, int v)
    {
        if (wide())
            put16(d, v);
        else
            d.push_back((uint8_t)v);
    }

    void parse()
    {
        while (!in_.empty()) {
            const uint8_t command = in_.front();
            const size_t size = writeSize(command);
            if (size) {
                if (in_.size() < size + 2)
                    return;
                std::vector<uint8_t> frame(in_.begin(), in_.begin() + size + 2);
                in_.erase(in_.begin(), in_.begin() + size + 2);
                writes.push_back(frame);
                if (relay::checksum(frame.data() + 1, size) != frame[size + 1])
                    continue;
                const int value = size == 2 ? (frame[1] << 8) | frame[2] : frame[1];
                if (command == 0x73) {
                    if (frame[1] == 3)
                        minPassMs = (frame[2] << 8) | frame[3];
                    continue;
                }
                if (command == 0x75) {
                    nodeName.assign((const char*)&frame[1], strnlen((const char*)&frame[1], 32));
                    continue;
                }
                if (command == 0x74) {
                    calibration.assign(frame.begin() + 1, frame.begin() + 37);
                    continue;
                }
                if (command == relay::WRITE_CURNODE_INDEX) {
                    if (value < (int)seats.size())
                        current_ = value;
                } else if (command == relay::WRITE_FREQUENCY) {
                    seats[current_].frequency = value;
                } else if (command == relay::WRITE_ENTER_AT_LEVEL) {
                    seats[current_].enter = value;
                } else if (command == relay::WRITE_EXIT_AT_LEVEL) {
                    seats[current_].exit = value;
                }
                continue;
            }
            in_.pop_front();
            answer(command);
        }
    }

    void answer(uint8_t command)
    {
        FakeSeat& s = seats[current_];
        std::vector<uint8_t> d;
        // The node answers halfway through the reply delay.
        const int64_t answeredUs = clockUs + replyDelayUs / 2;
        switch (command) {
        case relay::READ_REVISION_CODE: d = {relay::REVISION_MAGIC, (uint8_t)api}; break;
        case relay::READ_MULTINODE_COUNT: d = {(uint8_t)seats.size()}; break;
        case relay::READ_CURNODE_INDEX: d = {(uint8_t)current_}; break;
        case relay::READ_FW_VERSION:
        case relay::READ_FW_PROCTYPE: {
            const std::string& text = command == relay::READ_FW_VERSION ? firmware : proc;
            d.assign(relay::FW_TEXT_BLOCK_SIZE, 0);
            memcpy(d.data(), text.data(), std::min(text.size(), d.size()));
            break;
        }
        case relay::READ_FREQUENCY: put16(d, s.frequency); break;
        case relay::TEST_RX_REGISTER: d = {1}; break;
        case relay::READ_ENTER_AT_LEVEL: putRssi(d, s.enter); break;
        case relay::READ_EXIT_AT_LEVEL: putRssi(d, s.exit); break;
        case relay::READ_LAP_PASS_STATS: {
            d.push_back((uint8_t)s.lapId);
            const int64_t since = s.lastCrossUs ? (answeredUs - s.lastCrossUs) / 1000 : 0xFFFF;
            put16(d, (int)std::min<int64_t>(since, 0xFFFF));
            putRssi(d, s.rssi);
            putRssi(d, s.rssi + 50);
            putRssi(d, s.rssi + 30);
            put16(d, 1500);
            break;
        }
        case relay::READ_LAP_EXTREMUMS:
            d.push_back(s.crossing ? relay::LAPSTATS_FLAG_CROSSING : 0);
            putRssi(d, s.rssi);
            putRssi(d, s.rssi);
            while (d.size() < relay::lapStatsSize(wide()))
                d.push_back(0);
            break;
        case 0x11:  // feature flags: C5 detection and calibration
            if (!c5)
                return;
            d = {(uint8_t)(hasStats ? 0x01 : 0x00), (uint8_t)(hasIdentity ? 0xE0 : 0x60)};
            break;
        case 0x41:  // C5 detection v3, 86 bytes
            if (!c5)
                return;
            d.assign(86, 0);
            d[0] = 3;
            d[1] = minPassMs ? 0x07 : 0x03;  // writable, volatile, managed once set
            d[2] = (uint8_t)(minPassMs >> 8);
            d[3] = (uint8_t)minPassMs;
            break;
        case 0x42:
            if (!c5)
                return;
            d = calibration;
            break;
        case 0x43:
            if (!c5 || !hasIdentity)
                return;
            d = nodeId;
            break;
        case 0x44:
            if (!c5 || !hasIdentity)
                return;
            d.assign(32, 0);
            memcpy(d.data(), nodeName.data(), std::min<size_t>(nodeName.size(), 32));
            break;
        case 0x45:
            if (!c5 || !hasStats)
                return;
            d.assign(relay::NODE_STATS_SIZE, 0);
            d[0] = 1;
            d[1] = 1;  // power on
            d[2] = relay::NODE_STATS_UART_LINK;
            d[4] = (uint8_t)(uptimeS >> 24);
            d[5] = (uint8_t)(uptimeS >> 16);
            d[6] = (uint8_t)(uptimeS >> 8);
            d[7] = (uint8_t)uptimeS;
            break;
        default: return;  // the node ignores what it does not know
        }
        reply(command, d);
    }
};
