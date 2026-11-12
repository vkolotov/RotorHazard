// Drives a RotorHazard node (the C5 node, an S32_BPill, or one behind the
// AirPort link) the way RotorHazard's server does: start-up, then a sweep
// over the seats reading lap stats, reporting each new crossing once.
// A port of mach1's browser timer driver (frontend/src/lib/browserTimer/
// nodeDriver.ts), synchronous: one task owns the link and calls poll().
//
// Crossing time. RotorHazard takes reply time - half the round trip - "ms
// since crossing". The relay anchors on when the request was sent instead:
//   crossing = t_send + d_req - ms_since
// where d_req, the delay from sending to the node answering, is half the
// smallest round trip seen lately. A constant d_req cancels out of lap times;
// only its variation shows, and the send time has little.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace relay {

// The byte link to the node (a UART on the relay, a pty or a fake in tests).
class NodeLink {
public:
    virtual ~NodeLink() = default;
    virtual void flushInput() = 0;
    virtual void write(const uint8_t* data, size_t n) = 0;
    // Up to n bytes, waiting at most timeoutMs in total; returns how many came.
    virtual size_t read(uint8_t* data, size_t n, uint32_t timeoutMs) = 0;
    virtual int64_t nowUs() = 0;
    virtual void sleepMs(uint32_t ms) = 0;
};

// Over AirPort a timeout is a lost packet (AirPort never resends), not a late
// reply, so waiting longer only delays the retry. Replies take ~6.5 ms, the
// slowest seen ~12 ms. Lap times do not depend on it.
constexpr uint32_t REQUEST_TIMEOUT_MS = 50;
constexpr int MAX_RETRIES = 4;
constexpr uint32_t RETRY_PAUSE_MS = 5;
constexpr size_t RTT_WINDOW = 200;
// Consecutive failed seat reads before the link counts as lost.
constexpr int LOST_AFTER_FAILURES = 40;

struct NodeInfo {
    int apiLevel = 0;
    int seats = 0;
    std::string firmware;
    std::string procType;
    bool wide = false;  // 16-bit RSSI fields
};

struct SeatState {
    int frequency = 0;  // 0: off
    int enter = 0;
    int exit = 0;
    int rssi = 0;
    int passPeak = 0;
    int nodePeak = 0;
    bool crossing = false;
};

struct LinkCounters {
    uint32_t reads = 0;
    uint32_t writes = 0;
    uint32_t timeouts = 0;
    uint32_t checksumErrors = 0;
};

enum class NodeStatus { Closed, Syncing, Running, Lost };
const char* nodeStatusName(NodeStatus s);

struct Reply {
    std::vector<uint8_t> data;
    int64_t sentUs = 0;
    int64_t receivedUs = 0;
};

class NodeDriver {
public:
    explicit NodeDriver(NodeLink& link) : link_(link) {}

    // Sync with the node as RotorHazard does at connect. On failure the
    // status is Lost and `error` says why.
    bool start(std::string* error);
    // Read the next seat that is on. Call it in a loop while Running.
    // Returns false when every seat is off (nothing was read: pause a little).
    bool poll();

    // Put each seat on a frequency (0 switches it off); the seats that did not confirm.
    std::vector<int> setFrequencies(const std::vector<int>& frequencies);
    bool setSeatFrequency(int seat, int frequency);
    // New enter/exit levels for one seat (the node forgets them at power-off).
    bool setThresholds(int seat, int enter, int exit);

    // A completed crossing on `seat`, when it happened (link clock, µs).
    std::function<void(int seat, int64_t atUs, int peakRssi)> onCrossing;

    // Also read READ_LAP_EXTREMUMS for the "crossing now" flag. It doubles the
    // traffic per seat, so it is on only while mission control watches.
    bool readExtremums = false;

    NodeStatus status() const { return status_; }
    const std::string& detail() const { return detail_; }
    const NodeInfo& info() const { return info_; }
    const std::vector<SeatState>& seats() const { return seats_; }
    const LinkCounters& counters() const { return counters_; }
    double sweepsPerSecond() const { return sweepsPerSecond_; }
    int64_t minRttUs() const { return minRttUs_; }
    void markLost(const std::string& why);

    // Exposed for tests and the console.
    bool read(uint8_t command, size_t size, int retries, Reply* out);

private:
    bool readOnce(uint8_t command, size_t size, Reply* out, bool* gotSomething);
    void write(uint8_t command, const std::vector<uint8_t>& data);
    bool selectSeat(int seat);
    bool read16(uint8_t command, int* value);
    bool readRssi(uint8_t command, int* value);
    bool setAndValidate8(uint8_t write, uint8_t read, int value);
    bool setAndValidate16(uint8_t write, uint8_t read, int value);
    bool setAndValidateRssi(uint8_t write, uint8_t read, int value);
    bool setFrequency(int seat, int frequency);
    bool readSeat(int seat);
    void noteRtt(int64_t rttUs);
    void lapStats(int seat, int lapId, int64_t crossingUs, int peak);
    void resetLapTracking(int seat);

    NodeLink& link_;
    NodeInfo info_;
    std::vector<SeatState> seats_;
    NodeStatus status_ = NodeStatus::Closed;
    std::string detail_;
    LinkCounters counters_;
    int currentIndex_ = -1;
    int nextSeat_ = 0;
    int lastSeat_ = -1;
    int failures_ = 0;
    std::vector<int> lapIds_;
    struct Pending {
        bool set = false;
        int lapId = 0;
        int64_t atUs = 0;
    };
    std::vector<Pending> pending_;
    std::deque<int64_t> rtts_;
    int64_t minRttUs_ = 0;
    std::deque<int64_t> sweepTimes_;
    double sweepsPerSecond_ = 0;
};

}  // namespace relay
